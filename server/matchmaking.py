"""Queue pairing rules (pure functions, unit-tested without sockets).

Casual: first come, first served within region and ping limits.
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
    death_traps: bool = True  # the arenas' death traps; a casual match has them only when both want them, ranked never

    @property
    def key(self) -> tuple[str, str, bool]:
        return (self.game, self.mode, self.ranked)

    @property
    def queue_name(self) -> str:
        return f"{self.game}/{self.mode}/{'ranked' if self.ranked else 'casual'}"


@dataclass(slots=True)
class MatchmakingConfig:
    elo_base: int = 100
    elo_step: int = 50
    elo_step_seconds: float = 10.0
    elo_max: int = 600
    region_widen_after: float = 20.0
    allow_same_ip: bool = False


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


def ping_limit(a: QueueEntry, b: QueueEntry) -> int:
    """The tighter of the two max_ping settings; 0 means no limit."""
    limits = [p for p in (a.max_ping, b.max_ping) if p > 0]
    return min(limits) if limits else 0


def estimated_ping(a: QueueEntry, b: QueueEntry) -> int:
    if a.server_rtt is None or b.server_rtt is None:
        return 10_000
    return a.server_rtt + b.server_rtt


def compatible(a: QueueEntry, b: QueueEntry, now: float, cfg: MatchmakingConfig, cache: PingCache) -> bool:
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
    if measured is not None:
        if measured == UNREACHABLE:
            return False
        limit = ping_limit(a, b)
        if limit and measured > limit:
            return False
    if a.ranked and abs(a.elo - b.elo) > min(elo_window(a, now, cfg), elo_window(b, now, cfg)):
        return False
    return True


def find_pairs(
    entries: list[QueueEntry], now: float, cfg: MatchmakingConfig, cache: PingCache
) -> list[tuple[QueueEntry, QueueEntry]]:
    """Pairs to start, longest-waiting players first. Each entry appears in at most one pair."""
    ordered = sorted(entries, key=lambda e: (e.since, e.account_id))
    taken: set[int] = set()
    pairs: list[tuple[QueueEntry, QueueEntry]] = []
    for a in ordered:
        if a.account_id in taken:
            continue
        candidates = [
            b for b in ordered if b.account_id not in taken and compatible(a, b, now, cfg, cache)
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
