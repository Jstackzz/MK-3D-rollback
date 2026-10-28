"""The MK3D Online server: TLS/TCP NDJSON sessions, queues, matches, lobbies and the UDP rendezvous.

Everything runs on one asyncio event loop; SQLite is used synchronously from that loop (the
queries are tiny), password hashing runs in the default thread pool.
"""

from __future__ import annotations

import asyncio
import base64
import collections
import functools
import hmac
import itertools
import json
import logging
import os
import random
import secrets
import ssl
import time
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass, field
from typing import Any

from . import auth
from . import protocol as P
from . import upnp
from .db import Database, NameTaken
from .matchmaking import UNREACHABLE, MatchmakingConfig, PingCache, QueueEntry, find_pairs, max_ping_now

log = logging.getLogger("mk3d.server")

PENDING_STATES = ("found", "accepted", "punching")
NEEDS_LOGIN = {
    "logout", "queue_join", "queue_leave", "match_accept", "match_decline", "ping_report", "match_state",
    "result", "lobby_create", "lobby_list", "lobby_join", "lobby_leave", "lobby_chat", "lobby_character",
    "lobby_ready", "lobby_start", "admin_notice", "series_ready", "lobby_role", "feed", "lobby_settings",
}
FEED_MAGIC = b"MKDR"
FEED_TYPES = (5, 6, 7, 8, 9)  # the spectator feed's packet types (recomp/common/src/mk3d/netplay.cpp)
# a match without a result: the games parted (desync), lost each other (disconnected), the game failed (error), or
# the reporting player's game closed while the match ran (left: they ended it)
RESULT_OUTCOMES = ("desync", "disconnected", "error", "left")
BROKEN_OUTCOMES = ("disconnected", "error", "left")  # the two games are no longer together: no next match


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 7777
    udp_port: int = 7778
    db_path: str = "server.sqlite"
    certfile: str | None = "server-cert.pem"
    keyfile: str | None = "server-key.pem"
    plain: bool = False
    motd: str = ""
    admins: tuple[str, ...] = ()
    min_client: str = "0.1.0"
    # the release launchers are offered to update to (JSON, protocol.parse_update), read again when it changes; a
    # launcher older than it gets it in welcome, or pushed once it is announced
    update_file: str | None = None
    update_check_interval: float = 10.0
    accept_timeout: float = 20.0
    udp_timeout: float = 10.0
    punch_timeout: float = 10.0
    ping_report_timeout: float = 15.0
    result_timeout: float = 120.0
    abandon_wait: float = 20.0  # after a player's game lost the other, how long the other's report may take
    max_match_seconds: float = 3600.0
    idle_timeout: float = 120.0
    tick_seconds: float = 0.5
    online_push_interval: float = 2.0
    token_days: float = 30.0
    login_fail_limit: int = 10
    login_fail_window: float = 300.0
    chat_interval: float = 0.5
    default_rounds: int = 3
    set_wins: int = 2             # a ranked set goes to the first to win this many matches
    # casual: both choose a rematch within this, or the series ends; it starts with the result, and the launcher asks
    # once the game has played the match's end (the finisher, a fatality, the winner's pose: up to 30 s)
    rematch_timeout: float = 60.0
    set_next_timeout: float = 90.0  # ranked: both games ready for the set's next match within this, or the one
                                    # that is not loses the set (the last match's ending plays first)
    set_rejoin_grace: float = 20.0  # a player whose launcher drops between two matches has this long to be back
    lobby_rejoin_grace: float = 60.0  # a player whose connection drops gets their lobby place back if back this soon
    series_max_no_contest: int = 3  # matches in a row that do not count end the series (a set without Elo change)
    relay: bool = True           # two players who cannot reach each other play through this server's UDP port
    # every match through the relay (--relay-only): the players' games only ever talk to this server, so no player
    # learns another's address, at the cost of ping for players far from the server (two on the US west coast: ~20 ms
    # straight, ~140 ms through a server in Virginia). Otherwise only the matches of a player who asked to hide their
    # address ("relay" in queue_join / lobby_ready) go through it.
    relay_only: bool = False
    relay_rate: float = 400.0     # packets a second each player may send through the relay (a game sends ~120)
    relay_burst: float = 800.0
    relay_linger: float = 15.0    # seconds a route outlives its match: the last packets of the end still arrive
    max_spectators: int = 8       # a lobby's spectators, besides its two players
    feed_max_bytes: int = 8 << 20  # a lobby match's spectator feed, at most (a long chess game writes ~2 MB)
    feed_linger: float = 180.0    # seconds a feed outlives its match: its end still plays, spectators run behind
    upnp: bool = False            # open the TCP and UDP ports on the router of a home PC (UPnP)
    upnp_location: str | None = None  # development: a router description to use instead of discovering one
    upnp_renew: float = 1200.0
    scrypt_n: int = auth.SCRYPT_N
    mm: MatchmakingConfig = field(default_factory=MatchmakingConfig)


class Session:
    """One TCP connection."""

    _ids = itertools.count(1)

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.id = next(self._ids)
        self.reader = reader
        self.writer = writer
        peer = writer.get_extra_info("peername") or ("?", 0)
        self.ip: str = peer[0]
        self.hello = False
        self.client_version = ""
        self.account_id: int | None = None
        self.name: str | None = None
        self.admin = False
        self.token_hash: str | None = None
        self.udp_token: str | None = None
        self.udp_public: tuple[str, int] | None = None
        self.udp_lan = ""
        self.udp_seen = 0.0
        self.queue: QueueEntry | None = None
        self.lobby: Lobby | None = None
        self.match: Match | None = None
        self.last_chat = 0.0
        self.closed = False

    def __repr__(self) -> str:
        return f"<Session {self.id} {self.name or '-'} {self.ip}>"

    def send(self, msg: dict[str, Any]) -> None:
        if self.closed:
            return
        try:
            self.writer.write(P.encode(msg))
            transport = self.writer.transport
            if transport is not None and transport.get_write_buffer_size() > 1 << 20:
                log.warning("%r is not reading; closing", self)
                self.close()
        except (ConnectionError, RuntimeError, OSError):
            self.close()

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            with suppress(Exception):
                self.writer.close()


@dataclass(eq=False)
class MatchPlayer:
    side: int
    account_id: int
    name: str
    session: Session | None
    character: str
    delay: int
    max_ping: int
    region: str
    elo: int | None
    entry: QueueEntry | None = None
    accepted: bool = False
    relay: bool = False  # asked to hide their address: the match goes through this server's relay
    link: str = ""  # "ethernet", "wifi", "other" ("" unknown): shown to the opponent with match_found
    server_rtt: int | None = None  # their round trip to this server, ms
    ping: dict[str, Any] | None = None
    result: dict[str, Any] | None = None


@dataclass(eq=False)
class Match:
    id: str
    game: str
    mode: str
    ranked: bool
    players: list[MatchPlayer]
    created: float
    from_queue: bool
    lobby: Lobby | None = None
    rounds: int = 3
    state: str = "found"
    deadline: float = 0.0
    key: str = ""
    seed: int = 0
    rtt_ms: int = 0
    started: float | None = None
    game_state: str = ""
    relay: tuple[tuple[str, int], tuple[str, int]] | None = None  # the two endpoints the relay joins
    series: Series | None = None
    death_traps: bool = True

    def player_for(self, account_id: int | None) -> MatchPlayer | None:
        for p in self.players:
            if p.account_id == account_id:
                return p
        return None

    def other(self, p: MatchPlayer) -> MatchPlayer:
        return self.players[1] if p is self.players[0] else self.players[0]


@dataclass(eq=False)
class SeriesPlayer:
    account_id: int
    name: str
    side: int
    character: str
    delay: int
    max_ping: int
    region: str
    elo: int | None
    relay: bool = False
    wins: int = 0
    ready: bool = False
    choice: str = ""  # casual, once ready: "rematch" (the same fighters) or "select" (both pick again)


@dataclass(eq=False)
class Series:
    """The matches two players play one after another from a queue match. Casual: another one whenever both want a
    rematch. Ranked: a set, won by the first to win `need` matches; Elo moves once, at its end, and a player who
    leaves before then (declines the next match, lets its time run out, searches again) loses it."""
    id: str
    game: str
    mode: str
    ranked: bool
    need: int  # matches that win a ranked set; 0 casual
    players: list[SeriesPlayer]
    created: float
    state: str = "between"  # between matches (both must be ready for the next), playing, over
    deadline: float = 0.0
    match: Match | None = None
    played: int = 0
    no_contests: int = 0  # in a row
    last_picks: tuple[str, str] | None = None  # P1's and P2's fighters in the last agreed match (game character ids)
    death_traps: bool = True

    def player_for(self, account_id: int | None) -> SeriesPlayer | None:
        for p in self.players:
            if p.account_id == account_id:
                return p
        return None

    def other(self, p: SeriesPlayer) -> SeriesPlayer:
        return self.players[1] if p is self.players[0] else self.players[0]


@dataclass(eq=False)
class LobbyMember:
    session: Session
    character: str = ""
    ready: bool = False
    delay: int = 2
    spectator: bool = False  # watches the lobby's matches instead of playing them
    relay: bool = False  # asked to hide their address (with their ready): their matches go through the relay
    link: str = ""  # their PC's connection ("ethernet", "wifi", "other"; "" unknown), with their join or ready
    server_rtt: int | None = None  # their round trip to this server, ms, with their join or ready


@dataclass(eq=False)
class LobbySeat:
    """A lobby place kept for a player whose connection dropped: their next login within the grace takes it again."""
    lobby: Lobby
    character: str
    spectator: bool
    until: float


@dataclass(eq=False)
class Lobby:
    id: str
    name: str
    game: str
    mode: str
    region: str
    rounds: int
    password: str
    host: Session
    members: list[LobbyMember]
    created: float
    chat: collections.deque = field(default_factory=lambda: collections.deque(maxlen=30))
    match: Match | None = None
    death_traps: bool = True
    max_spectators: int = 8
    feed: Feed | None = None  # the spectator feed of the lobby's last match

    @property
    def state(self) -> str:
        if self.match is not None:
            return "in_match"
        return "full" if len(self.players()) >= 2 else "open"

    def players(self) -> list[LobbyMember]:
        return [m for m in self.members if not m.spectator]

    def spectators(self) -> list[LobbyMember]:
        return [m for m in self.members if m.spectator]

    def member(self, s: Session) -> LobbyMember | None:
        for m in self.members:
            if m.session is s:
                return m
        return None

    def summary(self) -> dict[str, Any]:
        return {
            "id": self.id, "name": self.name, "host": self.host.name, "game": self.game, "mode": self.mode,
            "region": self.region, "players": len(self.players()), "max_players": 2,
            "spectators": len(self.spectators()), "max_spectators": self.max_spectators,
            "locked": bool(self.password), "state": self.state, "rounds": self.rounds, "death_traps": self.death_traps,
        }

    def full_state(self) -> dict[str, Any]:
        return {
            "id": self.id, "name": self.name, "host": self.host.name, "game": self.game, "mode": self.mode,
            "region": self.region, "rounds": self.rounds, "locked": bool(self.password), "state": self.state,
            "death_traps": self.death_traps, "max_spectators": self.max_spectators,
            "match": self.match.id if self.match is not None else None,
            "members": [
                {"name": m.session.name, "character": m.character, "ready": m.ready, "host": m.session is self.host,
                 "spectator": m.spectator, "link": m.link or None, "ping": m.server_rtt}
                for m in self.members
            ],
            "chat": list(self.chat),
        }


@dataclass(eq=False)
class Feed:
    """What a lobby match's two games write for its spectators (docs/PROTOCOL.md 1.10.1), kept from the match's start
    until a while after it is done: the end still plays after the result, and spectators run behind the players."""
    match: str
    lobby: Lobby
    info: dict[str, Any]   # the spectate message
    sides: dict[int, int]  # account id -> the side byte its game's packets carry (0 P1, 1 P2)
    chunks: list[str] = field(default_factory=list)  # base64, each whole packets
    size: int = 0
    watchers: set[Session] = field(default_factory=set)
    end: dict[str, Any] | None = None  # the spectate_end message, once the match is done
    expires: float | None = None
    full: bool = False


@dataclass(eq=False)
class RelayRoute:
    """Where the relay sends what one player's endpoint sends: the other player's, for one match."""

    partner: tuple[str, int]
    match: str
    tokens: float
    last: float
    expires: float | None = None
    packets: int = 0
    dropped: int = 0


def _is_hello(data: bytes) -> bool:
    if data[:1] != b"{":
        return False
    try:
        msg = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return False
    return isinstance(msg, dict) and msg.get("type") == "hello"


class Rendezvous(asyncio.DatagramProtocol):
    """UDP endpoint: learns each client's public address from its hello and acknowledges it, and relays the
    packets of matches whose players cannot reach each other directly."""

    def __init__(self, server: Server) -> None:
        self.server = server
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        if self.transport is None:
            return
        route = self.server.relay_routes.get((addr[0], addr[1]))
        if route is not None and not _is_hello(data):
            self.server.relay_packet(route, data, self.transport)
            return
        if len(data) > P.MAX_DATAGRAM:
            return
        try:
            msg = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(msg, dict) or msg.get("type") != "hello" or not isinstance(msg.get("token"), str):
            return
        session = self.server.by_udp_token.get(msg["token"])
        if session is None or session.closed:
            return  # unknown tokens are never answered: no reflection
        self.server.on_udp_hello(session, (addr[0], addr[1]), msg)
        reply: dict[str, Any] = {"type": "hello_ack", "public": P.format_endpoint(addr)}
        t = msg.get("t")
        if isinstance(t, (int, float)) and not isinstance(t, bool):
            reply["t"] = t
        out = P.encode_datagram(reply)
        if len(out) <= len(data):  # never answer with more bytes than were sent
            self.transport.sendto(out, addr)

    def error_received(self, exc: Exception) -> None:
        log.debug("udp: %s", exc)


class Server:
    def __init__(self, cfg: ServerConfig | None = None) -> None:
        self.cfg = cfg or ServerConfig()
        self.db: Database | None = None
        self.rng = random.SystemRandom()
        self.sessions: set[Session] = set()
        self.by_account: dict[int, Session] = {}
        self.by_udp_token: dict[str, Session] = {}
        self.queue: dict[int, QueueEntry] = {}
        self.matches: dict[str, Match] = {}
        self.series: dict[str, Series] = {}
        self.series_of: dict[int, Series] = {}  # by account: a player is in one series at a time
        self.lobbies: dict[str, Lobby] = {}
        self.lobby_seats: dict[int, LobbySeat] = {}  # by account: kept while its launcher reconnects
        self.feeds: dict[str, Feed] = {}  # by match id
        self.relay_routes: dict[tuple[str, int], RelayRoute] = {}
        self.ping_cache = PingCache()
        self.login_failures: dict[str, collections.deque[float]] = {}
        self.tcp_port = self.cfg.port
        self.udp_port = self.cfg.udp_port
        self.fingerprint: str | None = None
        self.public_address = ""  # the router's, when UPnP opened the ports
        self._upnp_gateway: upnp.Gateway | None = None
        self._upnp_task: asyncio.Task | None = None
        self._tcp: asyncio.base_events.Server | None = None
        self._udp: asyncio.DatagramTransport | None = None
        self._ticker_task: asyncio.Task | None = None
        self._online_dirty = True
        self._online_last = 0.0
        self._online_payload_last: dict[str, Any] | None = None
        self._update: dict[str, Any] | None = None  # the update file's release (_latest_release)
        self._update_stamp: tuple[int, int] | None = None
        self._update_checked = 0.0
        self._update_pushed: str | None = None  # the release connected launchers were last offered
        self._dummy_hash: str | None = None
        self._handlers: dict[str, Callable[[Session, dict[str, Any]], Any]] = {
            "hello": self.on_hello,
            "register": self.on_register,
            "login": self.on_login,
            "auth": self.on_auth,
            "logout": self.on_logout,
            "profile": self.on_profile,
            "online": self.on_online,
            "ping": self.on_ping,
            "admin_notice": self.on_admin_notice,
            "queue_join": self.on_queue_join,
            "queue_leave": self.on_queue_leave,
            "match_accept": self.on_match_accept,
            "match_decline": self.on_match_decline,
            "ping_report": self.on_ping_report,
            "match_state": self.on_match_state,
            "result": self.on_result,
            "lobby_create": self.on_lobby_create,
            "lobby_list": self.on_lobby_list,
            "lobby_join": self.on_lobby_join,
            "lobby_leave": self.on_lobby_leave,
            "lobby_chat": self.on_lobby_chat,
            "lobby_character": self.on_lobby_character,
            "lobby_ready": self.on_lobby_ready,
            "lobby_start": self.on_lobby_start,
            "lobby_role": self.on_lobby_role,
            "lobby_settings": self.on_lobby_settings,
            "feed": self.on_feed,
            "series_ready": self.on_series_ready,
            "leaderboard": self.on_leaderboard,
        }

    # --- lifecycle --------------------------------------------------------------------------------

    def _ssl_context(self) -> ssl.SSLContext:
        from .makecert import fingerprint_of_pem

        if not self.cfg.certfile or not self.cfg.keyfile:
            raise FileNotFoundError("TLS needs --cert and --key (create them with: python -m server.makecert)")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(self.cfg.certfile, self.cfg.keyfile)
        self.fingerprint = fingerprint_of_pem(self.cfg.certfile)
        return ctx

    async def start(self) -> None:
        ssl_ctx = None if self.cfg.plain else self._ssl_context()
        self.db = Database(self.cfg.db_path)
        loop = asyncio.get_running_loop()
        self._tcp = await asyncio.start_server(
            self._handle, self.cfg.host, self.cfg.port, ssl=ssl_ctx, limit=P.MAX_LINE + 2
        )
        self.tcp_port = self._tcp.sockets[0].getsockname()[1]
        transport, _ = await loop.create_datagram_endpoint(
            lambda: Rendezvous(self), local_addr=(self.cfg.host, self.cfg.udp_port)
        )
        self._udp = transport  # type: ignore[assignment]
        self.udp_port = transport.get_extra_info("sockname")[1]
        self._update_pushed = (self._latest_release() or {}).get("version")  # nobody connected yet to offer it to
        self._ticker_task = asyncio.create_task(self._ticker(), name="mk3d-ticker")
        scheme = "tcp" if self.cfg.plain else "tls"
        log.info("MK3D server %s listening on %s://%s:%d, UDP rendezvous on %d, database %s",
                 P.SERVER_VERSION, scheme, self.cfg.host, self.tcp_port, self.udp_port, self.cfg.db_path)
        log.info("every match through the relay: players never see each other's address" if self.cfg.relay_only
                 else "players connect straight unless one asks to hide their address (then through the relay)")
        if self.fingerprint:
            log.info("certificate SHA-256 fingerprint %s", self.fingerprint)
        if self.cfg.upnp:
            self._upnp_task = asyncio.create_task(self._keep_ports_open(), name="mk3d-upnp")

    async def _keep_ports_open(self) -> None:
        """The server's ports on the router, renewed before their lease runs out."""
        loop = asyncio.get_running_loop()
        ports = [(self.tcp_port, "TCP"), (self.udp_port, "UDP")]
        while True:
            try:
                gateway, address = await loop.run_in_executor(None, upnp.open_ports, ports, self.cfg.upnp_location)
                self._upnp_gateway = gateway
                if address != self.public_address:
                    log.info("router (UPnP): TCP %d and UDP %d open; players connect to %s:%d", self.tcp_port,
                             self.udp_port, address or "this network's public address", self.tcp_port)
                self.public_address = address
            except (upnp.UpnpError, OSError) as exc:
                log.warning("router (UPnP): %s; forward TCP %d and UDP %d to this PC on the router by hand", exc,
                            self.tcp_port, self.udp_port)
            await asyncio.sleep(self.cfg.upnp_renew)

    async def _close_ports(self) -> None:
        gateway, self._upnp_gateway = self._upnp_gateway, None
        if gateway is None:
            return
        loop = asyncio.get_running_loop()

        def close() -> None:
            for port, protocol in ((self.tcp_port, "TCP"), (self.udp_port, "UDP")):
                with suppress(upnp.UpnpError, OSError):
                    upnp.delete_mapping(gateway, port, protocol)

        with suppress(Exception):
            await asyncio.wait_for(loop.run_in_executor(None, close), timeout=5)
        log.info("router (UPnP): ports closed")

    async def stop(self) -> None:
        if self._ticker_task is not None:
            self._ticker_task.cancel()
            with suppress(asyncio.CancelledError):
                await self._ticker_task
        if self._upnp_task is not None:
            self._upnp_task.cancel()
            with suppress(asyncio.CancelledError):
                await self._upnp_task
            await self._close_ports()
        if self._tcp is not None:
            self._tcp.close()
        for s in list(self.sessions):
            self._drop(s)  # leave queues, lobbies and matches while the database is still open
            s.close()
        if self._tcp is not None:
            with suppress(Exception):
                await asyncio.wait_for(self._tcp.wait_closed(), timeout=2)
        if self._udp is not None:
            self._udp.close()
        if self.db is not None:
            self.db.close()
            self.db = None

    async def _ticker(self) -> None:
        last_prune = time.time()
        while True:
            await asyncio.sleep(self.cfg.tick_seconds)
            now = time.time()
            try:
                self._tick(now)
                if now - last_prune > 60:
                    self._prune(now)
                    last_prune = now
            except Exception:  # never let one bad state kill the loop
                log.exception("tick failed")

    def _tick(self, now: float) -> None:
        for m in list(self.matches.values()):
            if m.state == "playing" and self._abandoned(m):
                self._finalize(m, now)
                continue
            if now < m.deadline:
                continue
            if m.state == "found":
                self._cancel_match(m, "accept_timeout", blame={p.side for p in m.players if not p.accepted})
            elif m.state == "accepted":
                self._cancel_match(m, "no_udp", blame={p.side for p in m.players if not self._fresh_udp(m, p)})
            elif m.state == "punching":
                self._cancel_match(m, "ping_timeout", blame={p.side for p in m.players if p.ping is None})
            elif m.state == "playing":
                self._finalize(m, now)
        self._tick_series(now)
        for ep in [ep for ep, r in self.relay_routes.items() if r.expires is not None and r.expires < now]:
            del self.relay_routes[ep]
        for feed in [f for f in self.feeds.values() if f.expires is not None and f.expires < now]:
            del self.feeds[feed.match]
            if feed.lobby.feed is feed:
                feed.lobby.feed = None
        self._run_matchmaking(now)
        self._push_online(now)
        self._push_update(now)

    def _prune(self, now: float) -> None:
        window = self.cfg.login_fail_window
        for ip in [ip for ip, q in self.login_failures.items() if not q or q[-1] < now - window]:
            del self.login_failures[ip]
        for account_id in [a for a, seat in self.lobby_seats.items() if seat.until < now]:
            del self.lobby_seats[account_id]
        self.ping_cache.prune(now)

    # --- connection handling ----------------------------------------------------------------------

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        s = Session(reader, writer)
        self.sessions.add(s)
        self._mark_online()
        log.debug("connect %r", s)
        try:
            while not s.closed:
                try:
                    line = await asyncio.wait_for(reader.readline(), timeout=self.cfg.idle_timeout)
                except TimeoutError:
                    s.send({"type": "error", "code": "idle", "message": "No traffic for too long."})
                    break
                except ValueError:  # line longer than the stream limit
                    s.send({"type": "error", "code": "too_long", "message": "Message too long."})
                    break
                except (ConnectionError, ssl.SSLError, OSError):
                    break
                if not line:
                    break
                if len(line) > P.MAX_LINE + 2:
                    s.send({"type": "error", "code": "too_long", "message": "Message too long."})
                    break
                line = line.strip()
                if line:
                    await self._dispatch(s, line)
                with suppress(Exception):
                    await writer.drain()
        finally:
            self._drop(s)
            s.close()
            with suppress(Exception):
                await asyncio.wait_for(writer.wait_closed(), timeout=2)

    def _drop(self, s: Session) -> None:
        if s not in self.sessions:
            return
        self.sessions.discard(s)
        if s.account_id is not None:
            log.info("disconnect %s", s.name)
        self._detach_account(s, keep_seat=True)
        self._mark_online()

    async def _dispatch(self, s: Session, line: bytes) -> None:
        msg: dict[str, Any] | None = None
        try:
            msg = P.decode_line(line)
            mtype = msg["type"]
            handler = self._handlers.get(mtype)
            if handler is None:
                raise P.ProtocolError("unknown_type", f"Unknown message type {mtype!r}.")
            if not s.hello and mtype != "hello":
                raise P.ProtocolError("need_hello", "Send hello first.")
            if mtype in NEEDS_LOGIN and s.account_id is None:
                raise P.ProtocolError("not_logged_in", "Log in first.")
            result = handler(s, msg)
            if asyncio.iscoroutine(result):
                await result
        except P.ProtocolError as exc:
            s.send(self._error(exc.code, exc.message, msg))
        except Exception:
            log.exception("handler failed for %r: %r", s, msg)
            s.send(self._error("internal", "Server error.", msg))

    @staticmethod
    def _error(code: str, message: str, request: dict[str, Any] | None) -> dict[str, Any]:
        out: dict[str, Any] = {"type": "error", "code": code, "message": message}
        if request is not None:
            out["re"] = request.get("type")
            if "id" in request:
                out["id"] = request["id"]
        return out

    @staticmethod
    def _reply(s: Session, request: dict[str, Any], msg: dict[str, Any]) -> None:
        if "id" in request:
            msg["id"] = request["id"]
        s.send(msg)

    async def _blocking(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, functools.partial(fn, *args, **kwargs))

    # --- hello, accounts --------------------------------------------------------------------------

    def on_hello(self, s: Session, msg: dict[str, Any]) -> None:
        if msg.get("proto") != P.PROTO:
            s.send({**self._error("proto_mismatch", f"This server speaks protocol {P.PROTO}.", msg),
                    "server_proto": P.PROTO})
            s.close()
            return
        version = str(msg.get("version", "0"))
        update = self._update_for(version)
        if not P.version_at_least(version, self.cfg.min_client):
            # launchers before 0.1.7 show only the message: it says where the release is
            page = update["page"] if update else P.RELEASES_PAGE
            reply = {**self._error("client_outdated", f"Update the launcher to {self.cfg.min_client} or newer: {page}",
                                   msg), "min_client": self.cfg.min_client}
            if update and P.version_at_least(update["version"], self.cfg.min_client):
                reply["update"] = update  # a launcher with the updater installs it from here
            s.send(reply)
            s.close()
            return
        s.hello = True
        s.client_version = version
        welcome = {
            "type": "welcome", "proto": P.PROTO, "server": P.SERVER_NAME, "version": P.SERVER_VERSION,
            "udp_port": self.udp_port, "motd": self.cfg.motd, "online": len(self.by_account),
            "min_client": self.cfg.min_client, "games": {g: list(m) for g, m in P.GAMES.items()},
            "regions": list(P.REGIONS),
        }
        if update:
            welcome["update"] = update
        self._reply(s, msg, welcome)

    # --- the latest release -----------------------------------------------------------------------

    def _latest_release(self) -> dict[str, Any] | None:
        """The update file's release, read again once the file changes; None without one (or a bad one)."""
        path = self.cfg.update_file
        if not path:
            return None
        try:
            st = os.stat(path)
            stamp: tuple[int, int] | None = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        if stamp != self._update_stamp:
            self._update_stamp = stamp
            self._update = None
            if stamp is not None:
                try:
                    with open(path, encoding="utf-8") as f:
                        self._update = P.parse_update(json.load(f))
                    log.info("update file: launchers older than %s are offered it", self._update["version"])
                except (OSError, ValueError) as exc:  # a JSON syntax error is a ValueError too
                    log.warning("update file %s ignored: %s", path, exc)
        return self._update

    def _update_for(self, version: str) -> dict[str, Any] | None:
        """The latest release when a launcher of this version is older."""
        latest = self._latest_release()
        if latest is None or P.version_at_least(version, latest["version"]):
            return None
        return latest

    def _push_update(self, now: float) -> None:
        """A release announced while launchers are connected: offered to the older ones at once."""
        if now - self._update_checked < self.cfg.update_check_interval:
            return
        self._update_checked = now
        latest = self._latest_release()
        version = latest["version"] if latest else None
        if version == self._update_pushed:
            return
        self._update_pushed = version
        if latest is None:
            return
        offered = 0
        for s in list(self.sessions):
            if s.hello and not P.version_at_least(s.client_version, latest["version"]):
                s.send({"type": "update", **latest})
                offered += 1
        log.info("release %s announced to %d connected launcher(s)", version, offered)

    def _check_rate(self, ip: str) -> None:
        q = self.login_failures.get(ip)
        if not q:
            return
        cutoff = time.time() - self.cfg.login_fail_window
        while q and q[0] < cutoff:
            q.popleft()
        if len(q) >= self.cfg.login_fail_limit:
            raise P.ProtocolError("rate_limited", "Too many failed logins; wait a few minutes.")

    def _failed_login(self, ip: str) -> None:
        self.login_failures.setdefault(ip, collections.deque()).append(time.time())

    async def on_register(self, s: Session, msg: dict[str, Any]) -> None:
        name = P.get_str(msg, "name", max_len=64)
        password = P.get_str(msg, "password", max_len=512)
        if s.account_id is not None:
            raise P.ProtocolError("busy", "Already logged in; log out first.")
        if not P.NAME_RE.match(name):
            raise P.ProtocolError("bad_name", "Names are 3-16 letters, digits, '_' or '-'.")
        if not P.PASSWORD_MIN <= len(password) <= P.PASSWORD_MAX:
            raise P.ProtocolError("bad_password", f"Passwords are {P.PASSWORD_MIN}-{P.PASSWORD_MAX} characters.")
        assert self.db is not None
        if self.db.account_by_name(name) is not None:
            raise P.ProtocolError("name_taken", "That name is taken.")
        pw_hash = await self._blocking(auth.hash_password, password, n=self.cfg.scrypt_n)
        if s.closed or self.db is None:
            return
        try:
            account_id = self.db.create_account(name, pw_hash)
        except NameTaken:
            raise P.ProtocolError("name_taken", "That name is taken.") from None
        log.info("register %s from %s", name, s.ip)
        self._login(s, self.db.account(account_id), msg)

    async def on_login(self, s: Session, msg: dict[str, Any]) -> None:
        name = P.get_str(msg, "name", max_len=64)
        password = P.get_str(msg, "password", max_len=512)
        if s.account_id is not None:
            raise P.ProtocolError("busy", "Already logged in; log out first.")
        self._check_rate(s.ip)
        assert self.db is not None
        row = self.db.account_by_name(name) if P.NAME_RE.match(name) else None
        if row is not None:
            ok = await self._blocking(auth.verify_password, password, row["pw_hash"])
        else:
            if self._dummy_hash is None:  # spend the same time for unknown names
                self._dummy_hash = await self._blocking(auth.hash_password, secrets.token_hex(8), n=self.cfg.scrypt_n)
            await self._blocking(auth.verify_password, password, self._dummy_hash)
            ok = False
        if s.closed or self.db is None:
            return
        if not ok:
            self._failed_login(s.ip)
            raise P.ProtocolError("bad_login", "Wrong name or password.")
        self._login(s, row, msg)

    def on_auth(self, s: Session, msg: dict[str, Any]) -> None:
        token = P.get_str(msg, "token", max_len=256)
        if s.account_id is not None:
            raise P.ProtocolError("busy", "Already logged in; log out first.")
        self._check_rate(s.ip)
        assert self.db is not None
        found = self.db.account_for_token(auth.token_hash(token)) if P.TOKEN_RE.match(token) else None
        if found is None:
            self._failed_login(s.ip)
            raise P.ProtocolError("bad_token", "Saved login expired; log in again.")
        row, expires = found
        self._login(s, row, msg, token=token, expires=expires)

    def _login(self, s: Session, row: Any, msg: dict[str, Any], *, token: str | None = None,
               expires: float | None = None) -> None:
        assert self.db is not None
        account_id = int(row["id"])
        old = self.by_account.get(account_id)
        if old is not None and old is not s:
            old.send({"type": "error", "code": "logged_in_elsewhere",
                      "message": "This account logged in from another place."})
            self._detach_account(old, keep_seat=True)  # a launcher back before the server saw it go: its place
            old.close()
        now = time.time()
        if token is None:
            token = auth.new_token()
            expires = now + self.cfg.token_days * 86400
            self.db.add_token(account_id, auth.token_hash(token), expires, now)
        self.db.touch_login(account_id, now)
        s.account_id = account_id
        s.name = row["name"]
        s.admin = row["name"].lower() in {a.lower() for a in self.cfg.admins}
        s.token_hash = auth.token_hash(token)
        s.udp_token = secrets.token_hex(16)
        self.by_udp_token[s.udp_token] = s
        self.by_account[account_id] = s
        self._reply(s, msg, {
            "type": "account", "name": s.name, "token": token, "token_expires": int(expires or 0),
            "udp_token": s.udp_token, "admin": s.admin, "profile": self._profile(row),
        })
        series = self.series_of.get(account_id)
        sp = series.player_for(account_id) if series is not None else None
        if series is not None and sp is not None:  # back in the middle of one
            s.send(self._series_msg(series, sp, now))
        self._mark_online()
        log.info("login %s from %s (launcher %s)", s.name, s.ip, s.client_version)
        self._rejoin_lobby(s, now)

    def _rejoin_lobby(self, s: Session, now: float) -> None:
        """Back within lobby_rejoin_grace of a dropped connection: the lobby takes the player in again (a player's
        place if one is free, else a spectator's), so a moment off the network does not cost them the lobby."""
        seat = self.lobby_seats.pop(s.account_id, None) if s.account_id is not None else None
        if seat is None or seat.until < now or s.lobby is not None or s.queue is not None or s.match is not None:
            return
        lobby = seat.lobby
        if self.lobbies.get(lobby.id) is not lobby:
            return
        spectator = seat.spectator or len(lobby.players()) >= 2
        if spectator and len(lobby.spectators()) >= lobby.max_spectators:
            return
        lobby.members.append(LobbyMember(s, seat.character, spectator=spectator))
        s.lobby = lobby
        log.info("lobby %s: %s back after a dropped connection%s", lobby.id, s.name, " to watch" if spectator else "")
        self._broadcast_lobby(lobby)
        if spectator and lobby.feed is not None and lobby.feed.end is None:
            self._watch(s, lobby.feed)

    def _detach_account(self, s: Session, keep_seat: bool = False) -> None:
        """Take a session out of queue, lobby and pending match, and forget its login. `keep_seat`: the connection
        dropped (not a logout or a leave), so the lobby place is kept a while for the same account."""
        if s.queue is not None:
            log.info("unqueue %s %s after %d s (signed out or gone)", s.name, s.queue.queue_name,
                     int(time.time() - s.queue.since))
            self.queue.pop(s.queue.account_id, None)
            s.queue = None
        if s.lobby is not None:
            member = s.lobby.member(s)
            if keep_seat and s.account_id is not None and member is not None:
                self.lobby_seats[s.account_id] = LobbySeat(s.lobby, member.character, member.spectator,
                                                           time.time() + self.cfg.lobby_rejoin_grace)
            self._leave_lobby(s)
        m = s.match
        if m is not None:
            s.match = None
            p = m.player_for(s.account_id)
            if p is not None:
                if m.state in PENDING_STATES:
                    self._cancel_match(m, "peer_left", blame={p.side})
                else:
                    p.session = None  # a playing match stays open for the other report
        if s.account_id is not None and self.by_account.get(s.account_id) is s:
            del self.by_account[s.account_id]
            # between two matches of a series the launcher has a moment to come back (a ranked set is lost otherwise)
            series = self.series_of.get(s.account_id)
            sp = series.player_for(s.account_id) if series is not None else None
            if series is not None and sp is not None and series.state == "between":
                sp.ready = False
                series.deadline = min(series.deadline, time.time() + self.cfg.set_rejoin_grace)
                self._send_series(series)
        if s.udp_token is not None:
            self.by_udp_token.pop(s.udp_token, None)
            s.udp_token = None
        s.account_id = None
        s.name = None
        s.admin = False
        s.token_hash = None
        self._mark_online()

    def on_logout(self, s: Session, msg: dict[str, Any]) -> None:
        assert self.db is not None
        if s.token_hash:
            self.db.delete_token(s.token_hash)
        log.info("logout %s", s.name)
        self._detach_account(s)
        self._reply(s, msg, {"type": "logged_out"})

    def _profile(self, row: Any) -> dict[str, Any]:
        assert self.db is not None
        return {"name": row["name"], "created": int(row["created"]), "stats": self.db.stats(int(row["id"]))}

    def on_profile(self, s: Session, msg: dict[str, Any]) -> None:
        assert self.db is not None
        name = msg.get("name")
        if name:
            if not isinstance(name, str) or not P.NAME_RE.match(name):
                raise P.ProtocolError("no_such_account", "No such player.")
            row = self.db.account_by_name(name)
            if row is None:
                raise P.ProtocolError("no_such_account", "No such player.")
        else:
            if s.account_id is None:
                raise P.ProtocolError("not_logged_in", "Log in first.")
            row = self.db.account(s.account_id)
        self._reply(s, msg, {"type": "profile", **self._profile(row)})

    # --- online count, notices, keepalive ---------------------------------------------------------

    def _online_payload(self) -> dict[str, Any]:
        queues = collections.Counter(e.queue_name for e in self.queue.values())
        playing = sum(len(m.players) for m in self.matches.values() if m.state != "found")
        return {"type": "online", "count": len(self.by_account), "connected": len(self.sessions),
                "queued": len(self.queue), "playing": playing, "queues": dict(sorted(queues.items()))}

    def _mark_online(self) -> None:
        self._online_dirty = True

    def _push_online(self, now: float) -> None:
        if not self._online_dirty or now - self._online_last < self.cfg.online_push_interval:
            return
        self._online_dirty = False
        payload = self._online_payload()
        if payload == self._online_payload_last:
            return
        self._online_last = now
        self._online_payload_last = payload
        for s in list(self.sessions):
            if s.hello:
                s.send(dict(payload))

    def on_online(self, s: Session, msg: dict[str, Any]) -> None:
        self._reply(s, msg, self._online_payload())

    def on_ping(self, s: Session, msg: dict[str, Any]) -> None:
        self._reply(s, msg, {"type": "pong", "t": time.time()})

    def on_admin_notice(self, s: Session, msg: dict[str, Any]) -> None:
        if not s.admin:
            raise P.ProtocolError("not_admin", "Only server admins can send notices.")
        text = P.get_str(msg, "text", max_len=500).strip()
        level = msg.get("level", "info")
        if not text or level not in ("info", "warn"):
            raise P.ProtocolError("bad_request", "Notice needs text and level info|warn.")
        self.broadcast_notice(text, level)
        log.info("notice from %s: %s", s.name, text)

    def broadcast_notice(self, text: str, level: str = "info") -> None:
        for other in list(self.sessions):
            if other.hello:
                other.send({"type": "notice", "text": text, "level": level})

    # --- queue ------------------------------------------------------------------------------------

    def on_queue_join(self, s: Session, msg: dict[str, Any]) -> None:
        assert self.db is not None and s.account_id is not None and s.name is not None
        game, mode = P.get_game_mode(msg)
        ranked = P.get_bool(msg, "ranked", default=False)
        region = P.get_region(msg)
        max_ping = P.get_int(msg, "max_ping", lo=0, hi=2000, default=0)
        strict = P.get_bool(msg, "strict", default=False)
        character = P.get_character(msg)
        delay = P.get_int(msg, "delay", lo=0, hi=P.DELAY_MAX, default=2)
        death_traps = P.get_bool(msg, "death_traps", default=True)
        relay = P.get_bool(msg, "relay", default=False)
        link = self._link(msg)
        server_rtt = msg.get("server_rtt")
        if isinstance(server_rtt, bool) or not isinstance(server_rtt, int) or not 0 <= server_rtt <= 5000:
            server_rtt = None
        if s.lobby is not None or s.match is not None:
            raise P.ProtocolError("busy", "Leave the lobby or finish the match first.")
        self._quit_series(s)
        now = time.time()
        rating = self.db.rating(s.account_id, game, mode)["elo"]
        entry = QueueEntry(
            account_id=s.account_id, name=s.name, game=game, mode=mode, ranked=ranked, region=region,
            max_ping=max_ping, strict=strict, character=character, delay=delay, since=now, elo=rating,
            public_ip=s.ip, server_rtt=server_rtt, death_traps=death_traps, relay=relay, link=link,
        )
        self.queue[s.account_id] = entry
        s.queue = entry
        waiting = sum(1 for e in self.queue.values() if e.key == entry.key)
        queued = {
            "type": "queued", "game": game, "mode": mode, "ranked": ranked, "region": region,
            "max_ping": max_ping, "strict": strict, "since": now, "in_queue": waiting, "elo": rating,
        }
        if max_ping > 0 and not strict:
            queued["ping_widen"] = self.cfg.mm.widen_rule()  # how the server raises it while nobody is found
        self._reply(s, msg, queued)
        log.info("queue %s %s (elo %d, %s, ping<=%s%s)", s.name, entry.queue_name, rating, region,
                 max_ping or "any", ", strict" if strict else "")
        self._mark_online()
        self._run_matchmaking(now)

    def on_queue_leave(self, s: Session, msg: dict[str, Any]) -> None:
        if s.queue is not None:
            log.info("unqueue %s %s after %d s", s.name, s.queue.queue_name, int(time.time() - s.queue.since))
            self.queue.pop(s.queue.account_id, None)
            s.queue = None
            self._mark_online()
        self._reply(s, msg, {"type": "queue_left", "reason": "left"})

    def _run_matchmaking(self, now: float) -> None:
        if len(self.queue) < 2:
            return
        for a, b in find_pairs(list(self.queue.values()), now, self.cfg.mm, self.ping_cache,
                               relayed=self.cfg.relay_only):
            self._create_queue_match(a, b, now)

    def _create_queue_match(self, a: QueueEntry, b: QueueEntry, now: float) -> None:
        sa, sb = self.by_account.get(a.account_id), self.by_account.get(b.account_id)
        for e, sess in ((a, sa), (b, sb)):
            self.queue.pop(e.account_id, None)
            if sess is not None:
                sess.queue = None
        if sa is None or sb is None:  # stale entry; put the live one back
            for e, sess in ((a, sa), (b, sb)):
                if sess is not None:
                    self.queue[e.account_id] = e
                    sess.queue = e
            return
        pair = [(a, sa), (b, sb)]
        self.rng.shuffle(pair)
        players = [
            MatchPlayer(side=i + 1, account_id=e.account_id, name=e.name, session=sess, character=e.character,
                        delay=e.delay, max_ping=max_ping_now(e, now, self.cfg.mm), region=e.region,
                        elo=e.elo if e.ranked else None, entry=e, relay=e.relay, link=e.link,
                        server_rtt=e.server_rtt)
            for i, (e, sess) in enumerate(pair)
        ]
        # the arenas' death traps: never in ranked; in casual the choice both searches made (they meet only when it
        # is the same, QueueEntry.key)
        traps = a.traps and b.traps
        m = Match(id=secrets.token_hex(6), game=a.game, mode=a.mode, ranked=a.ranked, players=players, created=now,
                  from_queue=True, rounds=self.cfg.default_rounds, state="found",
                  deadline=now + self.cfg.accept_timeout, death_traps=traps)
        self._register_match(m)
        for p in players:
            assert p.session is not None
            p.session.send({
                "type": "match_found", "match": m.id, "game": m.game, "mode": m.mode, "ranked": m.ranked,
                "side": p.side, "accept_timeout": self.cfg.accept_timeout, "accept_required": True, "lobby": None,
                "death_traps": m.death_traps, "opponent": None if m.ranked else self._opponent(m, m.other(p)),
                "connection": self._connection(m, p),
            })
        log.info("match %s found: %s vs %s (%s/%s %s)", m.id, players[0].name, players[1].name, m.game, m.mode,
                 "ranked" if m.ranked else "casual")

    def _register_match(self, m: Match) -> None:
        assert self.db is not None
        self.matches[m.id] = m
        for p in m.players:
            if p.session is not None:
                p.session.match = m
        self.db.create_match(m.id, m.game, m.mode, m.ranked, m.lobby.id if m.lobby else None,
                             m.players[0].account_id, m.players[1].account_id, m.created)
        self._mark_online()

    @staticmethod
    def _link(msg: dict[str, Any]) -> str:
        link = msg.get("link")
        return link if link in ("ethernet", "wifi", "other") else ""

    def _connection(self, m: Match, p: MatchPlayer) -> dict[str, Any]:
        """The other player's connection, as this one sees it with match_found (ranked too, without who it is): their
        PC's link, their round trip to this server, and the round trip between the two when known: measured by an
        earlier match of theirs, or both ways to this server when the match goes through its relay."""
        o = m.other(p)
        estimate = self.ping_cache.get(p.account_id, o.account_id, time.time())
        if estimate == UNREACHABLE:
            estimate = None
        measured = estimate is not None
        relayed = self.cfg.relay_only or p.relay or o.relay
        if estimate is None and relayed and p.server_rtt is not None and o.server_rtt is not None:
            estimate = p.server_rtt + o.server_rtt
        return {"link": o.link or None, "ping": o.server_rtt, "estimate": estimate, "measured": measured}

    @staticmethod
    def _opponent(m: Match, o: MatchPlayer) -> dict[str, Any]:
        return {"name": o.name, "elo": o.elo if m.ranked else None, "region": o.region, "character": o.character}

    # --- match flow -------------------------------------------------------------------------------

    def _my_match(self, s: Session, msg: dict[str, Any]) -> tuple[Match, MatchPlayer]:
        m = s.match
        if m is None or msg.get("match") != m.id:
            raise P.ProtocolError("no_such_match", "No such match.")
        p = m.player_for(s.account_id)
        if p is None:
            raise P.ProtocolError("no_such_match", "No such match.")
        return m, p

    def on_match_accept(self, s: Session, msg: dict[str, Any]) -> None:
        m, p = self._my_match(s, msg)
        if m.state != "found":
            return
        p.accepted = True
        if all(pl.accepted for pl in m.players):
            now = time.time()
            m.state = "accepted"
            m.deadline = now + self.cfg.udp_timeout
            self._maybe_send_peers(m, now)

    def on_match_decline(self, s: Session, msg: dict[str, Any]) -> None:
        m, p = self._my_match(s, msg)
        if m.state in PENDING_STATES:
            self._cancel_match(m, "declined", blame={p.side})

    def _fresh_udp(self, m: Match, p: MatchPlayer) -> bool:
        sess = p.session
        return sess is not None and sess.udp_public is not None and sess.udp_seen >= m.created

    def on_udp_hello(self, s: Session, addr: tuple[str, int], msg: dict[str, Any]) -> None:
        now = time.time()
        s.udp_public = addr
        s.udp_lan = P.format_endpoint(P.parse_endpoint(msg.get("lan")))
        s.udp_seen = now
        rtt = msg.get("rtt")  # the launcher's last round trip to this port: a search's estimate of a relayed match
        if s.queue is not None and isinstance(rtt, int) and not isinstance(rtt, bool) and 0 <= rtt <= 5000:
            s.queue.server_rtt = rtt
        m = s.match
        if m is not None and m.state == "accepted":
            self._maybe_send_peers(m, now)

    def _maybe_send_peers(self, m: Match, now: float) -> None:
        if m.state != "accepted" or not all(self._fresh_udp(m, p) for p in m.players):
            return
        m.state = "punching"
        m.key = secrets.token_hex(16)
        m.deadline = now + self.cfg.ping_report_timeout
        a, b = m.players
        assert a.session is not None and b.session is not None
        assert a.session.udp_public is not None and b.session.udp_public is not None
        if self.cfg.relay_only or any(p.relay for p in m.players):
            self._start_relay(m, now)  # a player who hides their address: neither learns the other's
            return
        same_nat = a.session.udp_public[0] == b.session.udp_public[0]
        for p in m.players:
            o = m.other(p)
            assert p.session is not None and o.session is not None
            p.session.send({
                "type": "match_peer", "match": m.id, "side": p.side, "key": m.key,
                "punch_timeout": self.cfg.punch_timeout, "same_nat": same_nat,
                # the home network's address only to a player behind the same router: nobody else can use it
                "peer": {"public": P.format_endpoint(o.session.udp_public),
                         "lan": o.session.udp_lan if same_nat else ""},
                "opponent": self._opponent(m, o),
            })
        log.info("match %s punching: %s %s / %s %s", m.id, a.name, P.format_endpoint(a.session.udp_public),
                 b.name, P.format_endpoint(b.session.udp_public))

    def on_ping_report(self, s: Session, msg: dict[str, Any]) -> None:
        m, p = self._my_match(s, msg)
        if m.state != "punching":
            return
        ok = P.get_bool(msg, "ok")
        rtt = P.get_int(msg, "rtt_ms", lo=0, hi=60_000, default=0)
        remote = P.format_endpoint(P.parse_endpoint(msg.get("remote")))
        via = msg.get("via") if msg.get("via") in ("lan", "public", "relay") else ""
        p.ping = {"ok": ok and bool(remote), "rtt_ms": rtt, "remote": remote, "via": via}
        if all(pl.ping is not None for pl in m.players):
            self._decide(m, time.time())

    def _decide(self, m: Match, now: float) -> None:
        a, b = m.players
        assert a.ping is not None and b.ping is not None
        if not (a.ping["ok"] and b.ping["ok"]):
            if self.cfg.relay and m.relay is None and all(self._fresh_udp(m, p) for p in m.players):
                self._start_relay(m, now)
                return
            self.ping_cache.put_unreachable(a.account_id, b.account_id, now)
            self._cancel_match(m, "punch_failed")
            return
        rtt = max(a.ping["rtt_ms"], b.ping["rtt_ms"])
        self.ping_cache.put(a.account_id, b.account_id, rtt, now)
        limits = [p.max_ping for p in m.players if p.max_ping > 0]
        if limits and rtt > min(limits):
            self._cancel_match(m, "ping_too_high")
            return
        self._go(m, rtt, now)

    def _start_relay(self, m: Match, now: float) -> None:
        """The two cannot reach each other (or relay_only): this server's UDP port carries their packets from now
        on, between the endpoints their hellos came from. Their launchers punch and ping through it as they would
        each other, and the games send to it."""
        a, b = m.players
        assert a.session is not None and b.session is not None
        ea, eb = a.session.udp_public, b.session.udp_public
        assert ea is not None and eb is not None
        m.relay = (ea, eb)
        m.state = "punching"
        m.deadline = now + self.cfg.ping_report_timeout
        for p in m.players:
            p.ping = None
        for src, dst in ((ea, eb), (eb, ea)):
            self.relay_routes[src] = RelayRoute(dst, m.id, tokens=self.cfg.relay_burst, last=time.monotonic())
        for p in m.players:
            o = m.other(p)
            assert p.session is not None
            p.session.send({
                "type": "match_peer", "match": m.id, "side": p.side, "key": m.key,
                "punch_timeout": self.cfg.punch_timeout, "same_nat": False, "relay": True,
                "peer": {"public": "", "lan": ""}, "opponent": self._opponent(m, o),
            })
        log.info("match %s relayed through this server: %s %s / %s %s", m.id, a.name, P.format_endpoint(ea),
                 b.name, P.format_endpoint(eb))

    def relay_packet(self, route: RelayRoute, data: bytes, transport: asyncio.DatagramTransport) -> None:
        """Passes one packet on, byte for byte: never more than came in, only between a match's two players, and
        no faster than relay_rate."""
        if len(data) > P.MAX_RELAY_DATAGRAM:
            route.dropped += 1
            return
        now = time.monotonic()
        route.tokens = min(self.cfg.relay_burst, route.tokens + (now - route.last) * self.cfg.relay_rate)
        route.last = now
        if route.tokens < 1.0:
            route.dropped += 1
            return
        route.tokens -= 1.0
        route.packets += 1
        transport.sendto(data, route.partner)

    def _end_relay(self, m: Match) -> None:
        if m.relay is None:
            return
        expires = time.time() + self.cfg.relay_linger
        counts = []
        for ep in m.relay:
            route = self.relay_routes.get(ep)
            if route is not None and route.match == m.id:
                route.expires = expires
                counts.append(f"{P.format_endpoint(ep)} sent {route.packets} ({route.dropped} dropped)")
        log.info("match %s relay: %s", m.id, ", ".join(counts))

    def _go(self, m: Match, rtt: int, now: float) -> None:
        assert self.db is not None
        m.state = "playing"
        m.started = now
        m.rtt_ms = rtt
        m.seed = self.rng.getrandbits(32)
        m.deadline = now + self.cfg.max_match_seconds
        delay = max(p.delay for p in m.players)
        if m.ranked:
            delay = max(1, delay)
        p1, p2 = m.players
        for p in m.players:
            assert p.session is not None and p.ping is not None
            p.session.send({
                "type": "match_go", "match": m.id, "game": m.game, "mode": m.mode, "ranked": m.ranked,
                "side": p.side, "remote": p.ping["remote"], "rtt_ms": rtt, "delay": delay, "seed": m.seed,
                "rounds": m.rounds, "p1": p1.character or "-", "p2": p2.character or "-",
                "p1_name": p1.name, "p2_name": p2.name, "lobby": m.lobby.id if m.lobby else None,
                "death_traps": m.death_traps, "feed": m.lobby is not None,
            })
        if m.lobby is not None:
            self._start_feed(m)
        self.db.update_match(m.id, status="playing", started=now, seed=m.seed, rtt_ms=rtt)
        log.info("match %s go: %s vs %s, rtt %d ms, delay %d%s", m.id, p1.name, p2.name, rtt, delay,
                 ", through the relay" if m.relay is not None else "")
        self._mark_online()

    def _cancel_match(self, m: Match, reason: str, blame: set[int] | frozenset[int] = frozenset()) -> None:
        if m.state in ("cancelled", "done"):
            return
        assert self.db is not None
        m.state = "cancelled"
        self.matches.pop(m.id, None)
        self._end_relay(m)
        now = time.time()
        for p in m.players:
            sess = p.session
            if sess is None:
                continue
            if sess.match is m:
                sess.match = None
            if sess.closed or sess.account_id != p.account_id:
                continue
            requeued = False
            if m.from_queue and p.side not in blame and p.entry is not None and sess.lobby is None:
                self.queue[p.account_id] = p.entry  # original join time is kept
                sess.queue = p.entry
                requeued = True
            sess.send({"type": "match_cancelled", "match": m.id, "reason": reason, "requeued": requeued})
            if m.from_queue and p.side in blame:
                sess.send({"type": "queue_left", "reason": "removed"})
        self.db.update_match(m.id, status="cancelled", cancel_reason=reason, ended=now)
        log.info("match %s cancelled: %s", m.id, reason)
        if m.lobby is not None:
            self._lobby_match_over(m.lobby, m)
        elif m.series is not None:
            self._series_match_cancelled(m, blame, now)
        self._mark_online()

    def on_match_state(self, s: Session, msg: dict[str, Any]) -> None:
        m = s.match
        if m is None or msg.get("match") != m.id:
            return
        state = msg.get("state")
        if isinstance(state, str) and len(state) <= 32:
            m.game_state = state

    # --- results ----------------------------------------------------------------------------------

    def on_result(self, s: Session, msg: dict[str, Any]) -> None:
        assert self.db is not None
        match_id = P.get_str(msg, "match", max_len=64)
        m = self.matches.get(match_id)
        if m is None or m.player_for(s.account_id) is None:
            row = self.db.match_row(match_id)
            if row is not None and s.account_id in (row["p1"], row["p2"]) and row["status"] not in PENDING_STATES:
                self._reply(s, msg, {"type": "result_ack", "match": match_id})  # late duplicate
                return
            raise P.ProtocolError("no_such_match", "No such match.")
        if m.state != "playing":
            raise P.ProtocolError("bad_request", "That match has not started.")
        p = m.player_for(s.account_id)
        assert p is not None
        outcome = msg.get("outcome")
        if outcome is not None:
            if outcome not in RESULT_OUTCOMES:
                raise P.ProtocolError("bad_request", "outcome must be desync, disconnected, error or left.")
            frame = msg.get("frame")
            report: dict[str, Any] = {"outcome": outcome,
                                      "frame": frame if isinstance(frame, int) and not isinstance(frame, bool) else None}
        else:
            report = {
                "winner": P.get_int(msg, "winner", lo=1, hi=2),
                "p1_rounds": P.get_int(msg, "p1_rounds", lo=0, hi=99),
                "p2_rounds": P.get_int(msg, "p2_rounds", lo=0, hi=99),
            }
            with suppress(P.ProtocolError):  # optional, never a reason to refuse a result
                fighters = (P.get_character(msg, "p1_char"), P.get_character(msg, "p2_char"))
                if all(fighters):  # what a casual rematch with the same fighters starts with
                    report["fighters"] = fighters
        now = time.time()
        p.result = report
        self.db.add_report(m.id, p.account_id, p.side, report, now)
        if s.match is m:
            s.match = None  # free to queue again while the other report is outstanding
        self._reply(s, msg, {"type": "result_ack", "match": m.id})
        if all(pl.result is not None for pl in m.players) or report.get("outcome") == "left" or self._abandoned(m):
            self._finalize(m, now)  # a player who closed their game ended the match for both, at once
        elif report.get("outcome") in BROKEN_OUTCOMES:
            m.deadline = min(m.deadline, now + self.cfg.abandon_wait)
        else:
            m.deadline = min(m.deadline, now + self.cfg.result_timeout)

    def _abandoned(self, m: Match) -> bool:
        """One game lost the other while that player is gone from the server too: no report will come from them."""
        lost = any(p.result is not None and p.result.get("outcome") in BROKEN_OUTCOMES for p in m.players)
        missing = [p for p in m.players if p.result is None]
        return lost and len(missing) == 1 and self.by_account.get(missing[0].account_id) is None

    def _fault(self, m: Match) -> MatchPlayer | None:
        """Who ended a match that has no result: the one player whose game closed while it ran (left), or the one who
        sent no report and is gone from the server while the other's game lost them. None when it was no one's doing
        (the games parted or lost each other on the way, both left at once)."""
        left = [p for p in m.players if p.result is not None and p.result.get("outcome") == "left"]
        if len(left) == 1:
            return left[0]
        if not left and self._abandoned(m):
            return next(p for p in m.players if p.result is None)
        return None

    def _finalize(self, m: Match, now: float) -> None:
        assert self.db is not None
        m.state = "done"
        self.matches.pop(m.id, None)
        self._end_relay(m)
        p1, p2 = m.players
        r1, r2 = p1.result, p2.result
        winner = p1_rounds = p2_rounds = None
        fields: dict[str, Any] = {"ended": now}
        fault = None
        if r1 is None or r2 is None:
            status = "unconfirmed"
        elif "outcome" in r1 or "outcome" in r2:
            status = "no_contest"
        elif (r1["winner"], r1["p1_rounds"], r1["p2_rounds"]) == (r2["winner"], r2["p1_rounds"], r2["p2_rounds"]):
            status = "agreed"
            winner, p1_rounds, p2_rounds = r1["winner"], r1["p1_rounds"], r1["p2_rounds"]
        else:
            status = "disputed"
        if status in ("unconfirmed", "no_contest"):
            fault = self._fault(m)
            if fault is not None:
                status = "abandoned"  # one player ended it: their series is over, a ranked set lost
        # the two games are no longer together (one closed, they lost each other): no next match in a series
        broken = status == "abandoned" or any(r is None or r.get("outcome") in BROKEN_OUTCOMES for r in (r1, r2))
        if status == "agreed":
            w, lose = (p1, p2) if winner == 1 else (p2, p1)
            if m.ranked:  # the match counts; Elo moves when the set ends (_end_series)
                self.db.record_ranked_match(w.account_id, lose.account_id, m.game, m.mode, now)
            else:
                self.db.record_casual(w.account_id, lose.account_id, m.game, m.mode, now)
            fields.update(winner=winner, p1_rounds=p1_rounds, p2_rounds=p2_rounds)
            log.info("RESULT match=%s %s beat %s %s-%s (%s)", m.id, w.name, lose.name, p1_rounds, p2_rounds,
                     "ranked" if m.ranked else "casual")
        elif status == "disputed":
            log.warning("DISPUTE match=%s %s reported %s, %s reported %s; no rating change",
                        m.id, p1.name, r1, p2.name, r2)
        elif fault is not None:
            log.info("ABANDONED match=%s by %s (%s=%s %s=%s)", m.id, fault.name, p1.name, r1, p2.name, r2)
        else:
            log.info("%s match=%s %s=%s %s=%s; no rating change", status.upper(), m.id, p1.name, r1, p2.name, r2)
        fields["status"] = status
        self.db.update_match(m.id, **fields)
        out: dict[str, Any] = {"type": "match_result", "match": m.id, "status": status, "winner": winner,
                               "p1_rounds": p1_rounds, "p2_rounds": p2_rounds, "ranked": m.ranked,
                               "left": fault.name if fault else None, "left_side": fault.side if fault else None}
        for p in m.players:
            sess = self.by_account.get(p.account_id)
            if sess is not None:
                if sess.match is m:
                    sess.match = None
                sess.send(dict(out))
        self._end_feed(m, out, now)
        if m.lobby is not None:
            self._lobby_match_over(m.lobby, m)
        elif m.from_queue or m.series is not None:
            fighters = None
            if status == "agreed" and r1.get("fighters") == r2.get("fighters"):
                fighters = r1.get("fighters")
            self._series_after_match(m, status, winner, now, fighters, fault=fault, broken=broken)
        self._mark_online()

    # --- series: casual rematches, ranked sets ---------------------------------------------------

    def _series_msg(self, series: Series, p: SeriesPlayer, now: float) -> dict[str, Any]:
        o = series.other(p)
        return {
            "type": "series", "series": series.id, "game": series.game, "mode": series.mode, "ranked": series.ranked,
            "state": series.state, "need": series.need, "played": series.played, "opponent": o.name,
            "you": p.wins, "them": o.wins, "you_ready": p.ready, "them_ready": o.ready,
            "you_choice": p.choice, "them_choice": o.choice, "same_fighters": series.last_picks is not None,
            "seconds_left": max(0, round(series.deadline - now)) if series.state == "between" else None,
        }

    def _send_series(self, series: Series, extra: dict[str, Any] | None = None) -> None:
        now = time.time()
        for p in series.players:
            sess = self.by_account.get(p.account_id)
            if sess is not None:
                sess.send({**self._series_msg(series, p, now), **(extra or {})})

    def _new_series(self, m: Match, now: float) -> Series:
        for p in m.players:
            old = self.series_of.get(p.account_id)
            if old is not None:
                self._end_series(old, "void", now=now)
        series = Series(
            id=secrets.token_hex(6), game=m.game, mode=m.mode, ranked=m.ranked,
            need=self.cfg.set_wins if m.ranked else 0, created=now, death_traps=m.death_traps,
            players=[SeriesPlayer(account_id=p.account_id, name=p.name, side=p.side, character=p.character,
                                  delay=p.delay, max_ping=p.max_ping, region=p.region, elo=p.elo, relay=p.relay)
                     for p in m.players],
        )
        self.series[series.id] = series
        for p in series.players:
            self.series_of[p.account_id] = series
        log.info("%s %s: %s vs %s (%s/%s)", "set" if series.ranked else "series", series.id, series.players[0].name,
                 series.players[1].name, series.game, series.mode)
        return series

    def _series_after_match(self, m: Match, status: str, winner: int | None, now: float,
                            fighters: tuple[str, str] | None = None, *, fault: MatchPlayer | None = None,
                            broken: bool = False) -> None:
        series = m.series if m.series is not None else self._new_series(m, now)
        if series.state == "over":  # a player left it while the other's report was outstanding
            return
        series.match = None
        series.played += 1
        series.last_picks = fighters
        if fault is not None:
            # a player whose game closed mid-match (or who vanished) left the series: in ranked, whatever the score,
            # that loses the set, so quitting a match never saves a player from its loss
            sp = series.player_for(fault.account_id)
            if sp is not None:
                self._end_series(series, "forfeit" if series.ranked else "left", left=sp, now=now)
                return
        if broken:  # the two games lost each other: no next match, nothing counts
            self._end_series(series, "void" if series.ranked else "disconnected", now=now)
            return
        if status == "agreed" and winner in (1, 2):
            sp = series.player_for(m.players[winner - 1].account_id)
            if sp is not None:
                sp.wins += 1
            series.no_contests = 0
        else:
            series.no_contests += 1
        champion = next((p for p in series.players if series.need and p.wins >= series.need), None)
        # a player who searched again or went to a lobby while the other report was outstanding has left it
        gone = next((p for p in series.players if self._moved_on(p.account_id)), None)
        if champion is not None:
            self._end_series(series, "won", winner=champion, now=now)
        elif gone is not None:
            self._leave_series(series, gone, now)
        elif series.no_contests >= self.cfg.series_max_no_contest:
            self._end_series(series, "void", now=now)
        else:
            self._series_between(series, now)

    def _moved_on(self, account_id: int) -> bool:
        sess = self.by_account.get(account_id)
        return sess is not None and (sess.queue is not None or sess.lobby is not None or sess.match is not None)

    def _series_between(self, series: Series, now: float) -> None:
        series.state = "between"
        for p in series.players:
            p.ready = False
            p.choice = ""
        series.deadline = now + (self.cfg.set_next_timeout if series.ranked else self.cfg.rematch_timeout)
        self._send_series(series)

    def _series_match_cancelled(self, m: Match, blame: set[int] | frozenset[int], now: float) -> None:
        series = m.series
        if series is None or series.state == "over" or series.match is not m:
            return
        series.match = None
        gone = [series.player_for(p.account_id) for p in m.players if p.side in blame]
        if len(gone) == 1 and gone[0] is not None:
            self._leave_series(series, gone[0], now)  # the one who did not show up for it
            return
        series.no_contests += 1
        if series.no_contests >= self.cfg.series_max_no_contest:
            self._end_series(series, "void", now=now)
        else:
            self._series_between(series, now)

    def _series_picks(self, series: Series) -> tuple[str, str]:
        """P1's and P2's characters in a series' next match ("" picks on the select screen). Casual: the last match's
        fighters when both want a rematch, the select screen for both when either wants to select (or when the last
        match's fighters are not known: the characters they queued with). A ranked set keeps the queued ones."""
        p1, p2 = sorted(series.players, key=lambda p: p.side)
        if series.ranked:
            return p1.character, p2.character
        if all(p.choice == "rematch" for p in series.players):
            return series.last_picks or (p1.character, p2.character)
        return "", ""

    def _next_series_match(self, series: Series, now: float) -> None:
        sessions = [self.by_account.get(p.account_id) for p in series.players]
        if any(sess is None or sess.match is not None for sess in sessions):
            return  # one of them is not here: the deadline decides
        picks = self._series_picks(series)
        players = sorted(
            (MatchPlayer(side=p.side, account_id=p.account_id, name=p.name, session=sess,
                         character=picks[p.side - 1], delay=p.delay, max_ping=p.max_ping, region=p.region,
                         elo=p.elo if series.ranked else None, accepted=True, relay=p.relay)
             for p, sess in zip(series.players, sessions)),
            key=lambda mp: mp.side,
        )
        m = Match(id=secrets.token_hex(6), game=series.game, mode=series.mode, ranked=series.ranked, players=players,
                  created=now, from_queue=False, series=series, rounds=self.cfg.default_rounds, state="accepted",
                  deadline=now + self.cfg.udp_timeout, death_traps=series.death_traps)
        series.state = "playing"
        series.match = m
        self._register_match(m)
        for p in players:
            assert p.session is not None
            p.session.send({
                "type": "match_found", "match": m.id, "game": m.game, "mode": m.mode, "ranked": m.ranked,
                "side": p.side, "accept_timeout": 0, "accept_required": False, "lobby": None, "series": series.id,
                "death_traps": m.death_traps, "opponent": self._opponent(m, m.other(p)),
            })
        log.info("%s %s: match %s (%d played)", "set" if series.ranked else "series", series.id, m.id, series.played)
        self._send_series(series)

    def _leave_series(self, series: Series, p: SeriesPlayer, now: float) -> None:
        """A player leaves: a ranked set with a match counted is lost, whatever its score (one match won and gone
        gains nothing); one where nothing counted yet (the first match desynced) ends without a rating change; a
        casual series just ends."""
        if not series.ranked:
            self._end_series(series, "left", left=p, now=now)
        elif any(pl.wins for pl in series.players):
            self._end_series(series, "forfeit", left=p, now=now)
        else:
            self._end_series(series, "void", left=p, now=now)

    def _end_series(self, series: Series, outcome: str, *, winner: SeriesPlayer | None = None,
                    left: SeriesPlayer | None = None, now: float | None = None) -> None:
        """won (a set's winner reached its wins), forfeit (ranked: `left` left the set and loses it, or closed their
        game during a match of it), left (casual, the same), timeout (casual: no rematch in time; ranked: neither
        game came back), void (matches kept not counting, a set was left before any of its matches counted, or the
        two games lost each other), disconnected (casual: the two games lost each other)."""
        if series.state == "over":
            return
        assert self.db is not None
        now = now or time.time()
        series.state = "over"
        self.series.pop(series.id, None)
        for p in series.players:
            if self.series_of.get(p.account_id) is series:
                del self.series_of[p.account_id]
        if outcome == "forfeit" and left is not None:
            winner = series.other(left)
        extra: dict[str, Any] = {"outcome": outcome, "winner": winner.name if winner else None,
                                 "left": left.name if left else None}
        changes = None
        if series.ranked:
            if winner is not None:
                loser = series.other(winner)
                changes = self.db.record_ranked_set(winner.account_id, loser.account_id, series.game, series.mode, now)
                extra["elo"] = {p.name: {"before": changes[p.account_id][0], "after": changes[p.account_id][1],
                                         "delta": changes[p.account_id][1] - changes[p.account_id][0]}
                                for p in series.players}
            p1, p2 = sorted(series.players, key=lambda p: p.side)
            self.db.log_set(series.id, series.game, series.mode, p1.account_id, p2.account_id, series.created, now,
                            outcome, winner.account_id if winner else None, p1.wins, p2.wins, changes)
            log.info("SET %s over: %s, %s %d-%d %s%s", series.id, outcome, p1.name, p1.wins, p2.wins, p2.name,
                     f", {winner.name} wins it" if winner else ", no rating change")
        else:
            log.info("series %s over: %s after %d matches", series.id, outcome, series.played)
        m = series.match
        series.match = None
        if m is not None and m.state in PENDING_STATES:
            self._cancel_match(m, "series_over", blame={m.player_for(left.account_id).side} if left else set())
        self._send_series(series, extra)

    def _quit_series(self, s: Session) -> None:
        """Searching again or going to a lobby leaves the series the player is in (losing a ranked set)."""
        series = self.series_of.get(s.account_id) if s.account_id is not None else None
        p = series.player_for(s.account_id) if series is not None else None
        if series is not None and p is not None:
            self._leave_series(series, p, time.time())

    def on_series_ready(self, s: Session, msg: dict[str, Any]) -> None:
        series = self.series_of.get(s.account_id) if s.account_id is not None else None
        if series is None or msg.get("series") != series.id:
            raise P.ProtocolError("no_such_series", "That series is over.")
        p = series.player_for(s.account_id)
        assert p is not None
        now = time.time()
        if not P.get_bool(msg, "ready"):
            self._reply(s, msg, {"type": "series_left", "series": series.id})
            self._leave_series(series, p, now)
            return
        choice = msg.get("choice", "rematch")
        if choice not in ("rematch", "select"):
            raise P.ProtocolError("bad_request", "choice must be rematch or select.")
        if series.state == "between":
            if s.match is not None or s.lobby is not None or s.queue is not None:
                raise P.ProtocolError("busy", "Leave the queue, lobby or match first.")
            p.ready = True
            p.choice = choice
        self._reply(s, msg, {"type": "series_ack", "series": series.id})
        if series.state != "between":
            return
        if all(pl.ready for pl in series.players):
            self._next_series_match(series, now)
        else:
            self._send_series(series)

    def _tick_series(self, now: float) -> None:
        for series in list(self.series.values()):
            if series.state != "between" or now < series.deadline:
                continue
            waiting = [p for p in series.players if not p.ready]
            if series.ranked and len(waiting) == 1:
                self._leave_series(series, waiting[0], now)  # the other was there for the next match
            else:
                self._end_series(series, "timeout", now=now)

    # --- lobbies ----------------------------------------------------------------------------------

    def _new_lobby_code(self) -> str:
        while True:
            code = "".join(self.rng.choice(P.LOBBY_ALPHABET) for _ in range(P.LOBBY_CODE_LEN))
            if code not in self.lobbies:
                return code

    def _broadcast_lobby(self, lobby: Lobby, reply_to: tuple[Session, dict[str, Any]] | None = None) -> None:
        state = lobby.full_state()
        for member in lobby.members:
            out = {"type": "lobby", "lobby": state}
            if reply_to is not None and member.session is reply_to[0] and "id" in reply_to[1]:
                out["id"] = reply_to[1]["id"]
            member.session.send(out)

    def _lobby_match_over(self, lobby: Lobby, m: Match) -> None:
        if lobby.match is not m:
            return
        lobby.match = None
        for member in lobby.members:
            member.ready = False
        if lobby.id in self.lobbies:
            self._broadcast_lobby(lobby)

    # --- spectators: a lobby match's feed ----------------------------------------------------------

    def _start_feed(self, m: Match) -> None:
        """A lobby match goes: its spectators get what it is (their game sets the same match up) and, from now on,
        what both players' games write for them."""
        lobby = m.lobby
        assert lobby is not None
        p1, p2 = m.players
        info = {
            "type": "spectate", "match": m.id, "lobby": lobby.id, "game": m.game, "mode": m.mode, "seed": m.seed,
            "rounds": m.rounds, "death_traps": m.death_traps, "p1": p1.character or "-", "p2": p2.character or "-",
            "p1_name": p1.name, "p2_name": p2.name,
        }
        feed = Feed(match=m.id, lobby=lobby, info=info, sides={p1.account_id: 0, p2.account_id: 1})
        self.feeds[m.id] = feed
        lobby.feed = feed
        for member in lobby.spectators():
            self._watch(member.session, feed)

    def _watch(self, s: Session, feed: Feed) -> None:
        """A spectator starts watching a feed: what it is, then everything written so far (paced by the spectator's
        connection), then the rest as it comes."""
        if s in feed.watchers:
            return
        feed.watchers.add(s)
        s.send(dict(feed.info))
        if feed.chunks:
            asyncio.get_running_loop().create_task(self._send_backlog(s, feed, len(feed.chunks)))
        elif feed.end is not None:
            s.send(dict(feed.end))

    async def _send_backlog(self, s: Session, feed: Feed, upto: int) -> None:
        batch: list[bytes] = []
        size = 0

        def flush() -> None:
            nonlocal batch, size
            if batch:
                s.send({"type": "spectate_feed", "match": feed.match,
                        "data": base64.b64encode(b"".join(batch)).decode("ascii")})
            batch, size = [], 0

        for data in feed.chunks[:upto]:
            if s.closed or s not in feed.watchers:
                return
            raw = base64.b64decode(data)
            if size + len(raw) > 9 * 1024:
                flush()
                transport = s.writer.transport
                while transport is not None and transport.get_write_buffer_size() > 256 * 1024 and not s.closed:
                    await asyncio.sleep(0.05)
            batch.append(raw)
            size += len(raw)
        flush()
        if feed.end is not None and s in feed.watchers:
            s.send(dict(feed.end))

    def _unwatch(self, s: Session, lobby: Lobby) -> None:
        for feed in self.feeds.values():
            if feed.lobby is lobby:
                feed.watchers.discard(s)

    def _end_feed(self, m: Match, result: dict[str, Any], now: float) -> None:
        feed = self.feeds.get(m.id)
        if feed is None or feed.end is not None:
            return
        feed.end = {"type": "spectate_end", "match": m.id, "status": result.get("status"),
                    "winner": result.get("winner"), "p1_rounds": result.get("p1_rounds"),
                    "p2_rounds": result.get("p2_rounds")}
        feed.expires = now + self.cfg.feed_linger
        for s in list(feed.watchers):
            s.send(dict(feed.end))
        log.info("match %s feed: %d bytes, %d spectators", m.id, feed.size, len(feed.watchers))

    def on_feed(self, s: Session, msg: dict[str, Any]) -> None:
        """A player's launcher passes on what its game wrote for the spectators: whole packets of the game's feed,
        its own side's only (docs/PROTOCOL.md 1.10.1). Late ones for a feed gone are dropped without a word."""
        feed = self.feeds.get(P.get_str(msg, "match", max_len=64))
        if feed is None or s.account_id not in feed.sides:
            return
        data = P.get_str(msg, "data", max_len=P.MAX_LINE)
        try:
            raw = base64.b64decode(data, validate=True)
        except (ValueError, TypeError) as exc:
            raise P.ProtocolError("bad_request", "Feed data must be base64.") from exc
        side = feed.sides[s.account_id]
        at = 0
        while at < len(raw):
            n = int.from_bytes(raw[at:at + 2], "little") if at + 2 <= len(raw) else -1
            pkt = raw[at + 2:at + 2 + n] if n >= 0 else b""
            if n < 8 or len(pkt) != n or pkt[:4] != FEED_MAGIC or pkt[4] not in FEED_TYPES or pkt[5] != side:
                raise P.ProtocolError("bad_request", "Feed data must be whole packets of this player's game.")
            at += 2 + n
        if not raw:
            return
        if feed.size + len(raw) > self.cfg.feed_max_bytes:
            if not feed.full:
                log.warning("match %s feed is full (%d bytes): spectators get no more", feed.match, feed.size)
                feed.full = True
            return
        feed.chunks.append(data)
        feed.size += len(raw)
        out = {"type": "spectate_feed", "match": feed.match, "data": data}
        for w in list(feed.watchers):
            w.send(out)

    def _member_connection(self, member: LobbyMember, msg: dict[str, Any]) -> None:
        if "link" in msg:
            member.link = self._link(msg)
        rtt = msg.get("server_rtt")
        if isinstance(rtt, int) and not isinstance(rtt, bool) and 0 <= rtt <= 5000:
            member.server_rtt = rtt

    def _need_lobby(self, s: Session) -> Lobby:
        if s.lobby is None:
            raise P.ProtocolError("not_in_lobby", "You are not in a lobby.")
        return s.lobby

    def on_lobby_create(self, s: Session, msg: dict[str, Any]) -> None:
        if s.queue is not None or s.lobby is not None or s.match is not None:
            raise P.ProtocolError("busy", "Leave the queue, lobby or match first.")
        game, mode = P.get_game_mode(msg)
        self._quit_series(s)
        default_name = f"{s.name}'s lobby"
        name = P.get_str(msg, "name", default=default_name, max_len=128).strip()[: P.LOBBY_NAME_MAX] or default_name
        password = P.get_str(msg, "password", default="", max_len=64)
        rounds = P.get_int(msg, "rounds", lo=1, hi=9, default=self.cfg.default_rounds)
        if rounds not in P.ROUNDS_ALLOWED:
            raise P.ProtocolError("bad_request", "rounds must be 1, 3 or 5.")
        region = P.get_region(msg)
        character = P.get_character(msg)
        death_traps = P.get_bool(msg, "death_traps", default=True)
        host = LobbyMember(s, character)
        self._member_connection(host, msg)
        lobby = Lobby(id=self._new_lobby_code(), name=name, game=game, mode=mode, region=region, rounds=rounds,
                      password=password, host=s, members=[host], created=time.time(),
                      death_traps=death_traps, max_spectators=self.cfg.max_spectators)
        self.lobbies[lobby.id] = lobby
        s.lobby = lobby
        log.info("lobby %s created by %s (%s/%s%s)", lobby.id, s.name, game, mode, ", locked" if password else "")
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def on_lobby_list(self, s: Session, msg: dict[str, Any]) -> None:
        game = msg.get("game")
        if game is not None and game not in P.GAMES:
            raise P.ProtocolError("bad_game", f"Unknown game {game!r}.")
        items = [lb.summary() for lb in sorted(self.lobbies.values(), key=lambda lb: lb.created)
                 if game is None or lb.game == game]
        self._reply(s, msg, {"type": "lobbies", "lobbies": items})

    def on_lobby_join(self, s: Session, msg: dict[str, Any]) -> None:
        if s.queue is not None or s.lobby is not None or s.match is not None:
            raise P.ProtocolError("busy", "Leave the queue, lobby or match first.")
        code = P.get_str(msg, "lobby", max_len=16).strip().upper()
        lobby = self.lobbies.get(code)
        if lobby is None:
            raise P.ProtocolError("no_such_lobby", "No lobby with that code.")
        spectate = P.get_bool(msg, "spectate", default=False)
        if spectate and len(lobby.spectators()) >= lobby.max_spectators:
            raise P.ProtocolError("lobby_full", "That lobby has no room for more spectators.")
        if not spectate and (lobby.match is not None or len(lobby.players()) >= 2):
            raise P.ProtocolError("lobby_full", "Both places to play are taken: join it to watch instead.")
        password = P.get_str(msg, "password", default="", max_len=64)
        if lobby.password and not hmac.compare_digest(lobby.password.encode(), password.encode()):
            raise P.ProtocolError("bad_lobby_password", "Wrong lobby password.")
        self._quit_series(s)
        member = LobbyMember(s, P.get_character(msg), spectator=spectate)
        self._member_connection(member, msg)
        lobby.members.append(member)
        s.lobby = lobby
        log.info("lobby %s: %s joined%s", lobby.id, s.name, " to watch" if spectate else "")
        self._broadcast_lobby(lobby, reply_to=(s, msg))
        if spectate and lobby.feed is not None and lobby.feed.end is None:
            self._watch(s, lobby.feed)  # a match under way: from its start

    def _leave_lobby(self, s: Session) -> None:
        lobby = s.lobby
        if lobby is None:
            return
        s.lobby = None
        member = lobby.member(s)
        if member is not None:
            lobby.members.remove(member)
        self._unwatch(s, lobby)
        if not lobby.members:
            self.lobbies.pop(lobby.id, None)
        elif lobby.host is s:
            lobby.host = (lobby.players() or lobby.members)[0].session
        if member is None or not member.spectator:
            for other in lobby.members:
                other.ready = False
        log.info("lobby %s: %s left", lobby.id, s.name)
        m = lobby.match
        p = m.player_for(s.account_id) if m is not None else None
        if m is not None and p is not None and m.state in PENDING_STATES:
            self._cancel_match(m, "peer_left", blame={p.side})  # broadcasts the lobby
        elif lobby.members:
            self._broadcast_lobby(lobby)

    def on_lobby_leave(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        self._leave_lobby(s)
        self._reply(s, msg, {"type": "lobby_left", "lobby": lobby.id})

    def on_lobby_chat(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        text = " ".join(P.get_str(msg, "text", max_len=1000).split())
        if not text:
            raise P.ProtocolError("bad_request", "Empty message.")
        text = text[: P.CHAT_MAX]
        now = time.time()
        if now - s.last_chat < self.cfg.chat_interval:
            raise P.ProtocolError("slow_down", "You are sending messages too fast.")
        s.last_chat = now
        entry = {"from": s.name, "text": text, "ts": round(now, 3)}
        lobby.chat.append(entry)
        for member in lobby.members:
            out = {"type": "lobby_chat", "lobby": lobby.id, **entry}
            if member.session is s and "id" in msg:
                out["id"] = msg["id"]
            member.session.send(out)

    def on_lobby_character(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        if lobby.match is not None:
            raise P.ProtocolError("busy", "A match is running.")
        member = lobby.member(s)
        assert member is not None
        member.character = P.get_character(msg)
        member.ready = False
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def on_lobby_ready(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        if lobby.match is not None:
            raise P.ProtocolError("busy", "A match is running.")
        member = lobby.member(s)
        assert member is not None
        if member.spectator:
            raise P.ProtocolError("not_player", "Spectators do not ready up: take a place to play first.")
        member.ready = P.get_bool(msg, "ready")
        member.delay = P.get_int(msg, "delay", lo=0, hi=P.DELAY_MAX, default=member.delay)
        member.relay = P.get_bool(msg, "relay", default=False)
        self._member_connection(member, msg)
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def on_lobby_role(self, s: Session, msg: dict[str, Any]) -> None:
        """Play or watch: a spectator takes a free place to play, a player steps aside to watch (not while their
        match is being set up or runs)."""
        lobby = self._need_lobby(s)
        member = lobby.member(s)
        assert member is not None
        role = msg.get("role")
        if role not in ("player", "spectator"):
            raise P.ProtocolError("bad_request", "role must be player or spectator.")
        if (role == "spectator") == member.spectator:
            self._broadcast_lobby(lobby, reply_to=(s, msg))
            return
        if role == "player":
            if lobby.match is not None:
                raise P.ProtocolError("busy", "A match is running: take a place once it is over.")
            if len(lobby.players()) >= 2:
                raise P.ProtocolError("lobby_full", "Both places to play are taken.")
            member.spectator = False
            self._unwatch(s, lobby)
        else:
            if lobby.match is not None and lobby.match.player_for(s.account_id) is not None:
                raise P.ProtocolError("busy", "Your match is running.")
            if len(lobby.spectators()) >= lobby.max_spectators:
                raise P.ProtocolError("lobby_full", "That lobby has no room for more spectators.")
            member.spectator = True
            for other in lobby.members:
                other.ready = False
        member.ready = False
        log.info("lobby %s: %s %s", lobby.id, s.name, "plays" if role == "player" else "watches")
        self._broadcast_lobby(lobby, reply_to=(s, msg))
        if member.spectator and lobby.feed is not None and lobby.feed.end is None:
            self._watch(s, lobby.feed)

    def on_lobby_settings(self, s: Session, msg: dict[str, Any]) -> None:
        """The host changes the lobby's death traps between matches; both players ready again for the next one."""
        lobby = self._need_lobby(s)
        if lobby.host is not s:
            raise P.ProtocolError("not_host", "Only the host can change the lobby.")
        if lobby.match is not None:
            raise P.ProtocolError("busy", "A match is running.")
        death_traps = P.get_bool(msg, "death_traps", default=lobby.death_traps)
        if death_traps != lobby.death_traps:
            lobby.death_traps = death_traps
            for member in lobby.members:
                member.ready = False
            log.info("lobby %s: death traps %s", lobby.id, "on" if death_traps else "off")
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def on_lobby_start(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        if lobby.host is not s:
            raise P.ProtocolError("not_host", "Only the host can start.")
        if lobby.match is not None:
            raise P.ProtocolError("busy", "A match is already running.")
        if len(lobby.players()) < 2 or not all(m.ready for m in lobby.players()):
            raise P.ProtocolError("not_ready", "Both players must be ready.")
        now = time.time()
        ordered = sorted(lobby.players(), key=lambda mem: mem.session is not lobby.host)  # a host who plays first
        players = [
            MatchPlayer(side=i + 1, account_id=mem.session.account_id or 0, name=mem.session.name or "?",
                        session=mem.session, character=mem.character, delay=mem.delay, max_ping=0,
                        region=lobby.region, elo=None, accepted=True, relay=mem.relay)
            for i, mem in enumerate(ordered)
        ]
        m = Match(id=secrets.token_hex(6), game=lobby.game, mode=lobby.mode, ranked=False, players=players,
                  created=now, from_queue=False, lobby=lobby, rounds=lobby.rounds, state="accepted",
                  deadline=now + self.cfg.udp_timeout, death_traps=lobby.death_traps)
        lobby.match = m
        self._register_match(m)
        for p in players:
            assert p.session is not None
            p.session.send({
                "type": "match_found", "match": m.id, "game": m.game, "mode": m.mode, "ranked": False,
                "side": p.side, "accept_timeout": 0, "accept_required": False, "lobby": lobby.id,
                "death_traps": m.death_traps, "opponent": self._opponent(m, m.other(p)),
            })
        log.info("lobby %s: match %s started by %s", lobby.id, m.id, s.name)
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    # --- leaderboard ------------------------------------------------------------------------------

    def on_leaderboard(self, s: Session, msg: dict[str, Any]) -> None:
        assert self.db is not None
        game, mode = P.get_game_mode(msg)
        limit = P.get_int(msg, "limit", lo=1, hi=200, default=100)
        offset = P.get_int(msg, "offset", lo=0, hi=1_000_000, default=0)
        rows, total = self.db.leaderboard(game, mode, limit, offset)
        out: dict[str, Any] = {"type": "leaderboard", "game": game, "mode": mode, "total": total, "rows": rows}
        if s.account_id is not None:
            me = self.db.rank_of(s.account_id, game, mode)
            if me is not None:
                out["me"] = me
        self._reply(s, msg, out)
