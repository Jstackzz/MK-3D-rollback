"""Queue pairing rules (pure functions, unit-tested without sockets).

Casual: first come, first served within region and ping limits, among the searches that chose
the same death traps.
Ranked: additionally an Elo window that widens with waiting time; the longest-waiting player
gets the closest-rated compatible partner.
"""

from __future__ import annotations

from dataclasses import dataclass, field

UNREACHABLE = -1


@dataclass(slots=True)
class QueueEntry:
    account_id: int
    name: str
    game: str
    mode: str
    ranked: bool
    region: str
    max_ping: int  # ms, 0 = any
    strict: bool
    character: str
    delay: int
    since: float
    elo: int = 1500
    public_ip: str | None = None
    server_rtt: int | None = None  # client's UDP round trip to the server, ms
    death_traps: bool = True  # the arenas' death traps the player wants (ranked never has them)
    relay: bool = False  # hides their address: their matches go through the server's relay
    link: str = ""  # the PC's connection to its router: "ethernet", "wifi", "other" ("" unknown)

    @property
    def traps(self) -> bool:
        """The death traps a match from this search has: the player's choice in casual, never in ranked."""
        return self.death_traps and not self.ranked

    @property
    def key(self) -> tuple[str, str, bool, bool]:
        # a casual search meets only the searches that chose the same death traps
        return (self.game, self.mode, self.ranked, self.traps)

    @property
    def queue_name(self) -> str:
        name = f"{self.game}/{self.mode}/{'ranked' if self.ranked else 'casual'}"
        return name if self.ranked or self.death_traps else name + "/no-traps"


@dataclass(slots=True)
class MatchmakingConfig:
    elo_base: int = 100
    elo_step: int = 50
    elo_step_seconds: float = 10.0
    elo_max: int = 600
    region_widen_after: float = 20.0
    allow_same_ip: bool = False
    # a search that finds nobody raises its max ping (unless strict): after ping_widen_after seconds by
    # ping_widen_step ms, again every ping_widen_every seconds, up to ping_widen_cap ms
    ping_widen_after: float = 30.0
    ping_widen_every: float = 15.0
    ping_widen_step: int = 25
    ping_widen_cap: int = 250

    def widen_rule(self) -> dict[str, float | int]:
        return {"after": self.ping_widen_after, "every": self.ping_widen_every, "step": self.ping_widen_step,
                "cap": self.ping_widen_cap}


@dataclass
class PingCache:
    """Measured round trips per pair of accounts, from ping_report; expire after `ttl` seconds."""

    ttl: float = 600.0
    _data: dict[frozenset[int], tuple[int, float]] = field(default_factory=dict)

    def put(self, a: int, b: int, rtt_ms: int, now: float) -> None:
        self._data[frozenset((a, b))] = (int(rtt_ms), now + self.ttl)

    def put_unreachable(self, a: int, b: int, now: float) -> None:
        self._data[frozenset((a, b))] = (UNREACHABLE, now + self.ttl)

    def get(self, a: int, b: int, now: float) -> int | None:
        item = self._data.get(frozenset((a, b)))
        if item is None:
            return None
        value, expires = item
        if expires <= now:
            del self._data[frozenset((a, b))]
            return None
        return value

    def prune(self, now: float) -> None:
        for key in [k for k, (_, expires) in self._data.items() if expires <= now]:
            del self._data[key]

    def __len__(self) -> int:
        return len(self._data)


def elo_window(entry: QueueEntry, now: float, cfg: MatchmakingConfig) -> int:
    if entry.strict:
        return cfg.elo_base
    steps = int(max(0.0, now - entry.since) // cfg.elo_step_seconds)
    return min(cfg.elo_max, cfg.elo_base + cfg.elo_step * steps)


def max_ping_now(entry: QueueEntry, now: float, cfg: MatchmakingConfig) -> int:
    """A search's max_ping as waiting has raised it (MatchmakingConfig.ping_widen_*); 0 (any) and strict ones stay."""
    if entry.max_ping <= 0 or entry.strict:
        return entry.max_ping
    over = now - entry.since - cfg.ping_widen_after
    if over < 0:
        return entry.max_ping
    raised = entry.max_ping + cfg.ping_widen_step * (1 + int(over // cfg.ping_widen_every))
    return max(entry.max_ping, min(cfg.ping_widen_cap, raised))


def ping_limit(a: QueueEntry, b: QueueEntry, now: float = 0.0, cfg: MatchmakingConfig | None = None) -> int:
    """The tighter of the two max_ping limits (as waiting raised them, given `cfg`); 0 means no limit."""
    pair = (max_ping_now(a, now, cfg), max_ping_now(b, now, cfg)) if cfg else (a.max_ping, b.max_ping)
    limits = [p for p in pair if p > 0]
    return min(limits) if limits else 0


def estimated_ping(a: QueueEntry, b: QueueEntry) -> int:
    if a.server_rtt is None or b.server_rtt is None:
        return 10_000
    return a.server_rtt + b.server_rtt


def compatible(a: QueueEntry, b: QueueEntry, now: float, cfg: MatchmakingConfig, cache: PingCache,
               relayed: bool = False) -> bool:
    if a.account_id == b.account_id or a.key != b.key:
        return False
    if a.ranked and not cfg.allow_same_ip and a.public_ip and a.public_ip == b.public_ip:
        return False
    if a.region != b.region:
        if a.strict or b.strict:
            return False
        if now - a.since < cfg.region_widen_after or now - b.since < cfg.region_widen_after:
            return False
    measured = cache.get(a.account_id, b.account_id, now)
    if measured is None and (relayed or a.relay or b.relay) and a.server_rtt is not None and b.server_rtt is not None:
        # the match would go through the server: its round trip is each player's to the server, so a pair over a
        # player's limit is not offered a match that would be cancelled for its ping
        measured = estimated_ping(a, b)
    if measured is not None:
        if measured == UNREACHABLE:
            return False
        limit = ping_limit(a, b, now, cfg)
        if limit and measured > limit:
            return False
    if a.ranked and abs(a.elo - b.elo) > min(elo_window(a, now, cfg), elo_window(b, now, cfg)):
        return False
    return True


def find_pairs(
    entries: list[QueueEntry], now: float, cfg: MatchmakingConfig, cache: PingCache, relayed: bool = False
) -> list[tuple[QueueEntry, QueueEntry]]:
    """Pairs to start, longest-waiting players first. Each entry appears in at most one pair."""
    ordered = sorted(entries, key=lambda e: (e.since, e.account_id))
    taken: set[int] = set()
    pairs: list[tuple[QueueEntry, QueueEntry]] = []
    for a in ordered:
        if a.account_id in taken:
            continue
        candidates = [
            b for b in ordered if b.account_id not in taken and compatible(a, b, now, cfg, cache, relayed)
        ]
        if not candidates:
            continue
        if a.ranked:
            b = min(candidates, key=lambda c: (abs(a.elo - c.elo), estimated_ping(a, c), c.since))
        else:
            b = candidates[0]  # first come, first served
        taken.update((a.account_id, b.account_id))
        pairs.append((a, b))
    return pairs
