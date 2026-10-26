"""The MK3D Online server: TLS/TCP NDJSON sessions, queues, matches, lobbies and the UDP rendezvous.

Everything runs on one asyncio event loop; SQLite is used synchronously from that loop (the
queries are tiny), password hashing runs in the default thread pool.
"""

from __future__ import annotations

import asyncio
import collections
import functools
import hmac
import itertools
import json
import logging
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
from .matchmaking import MatchmakingConfig, PingCache, QueueEntry, find_pairs

log = logging.getLogger("mk3d.server")

PENDING_STATES = ("found", "accepted", "punching")
NEEDS_LOGIN = {
    "logout", "queue_join", "queue_leave", "match_accept", "match_decline", "ping_report", "match_state",
    "result", "lobby_create", "lobby_list", "lobby_join", "lobby_leave", "lobby_chat", "lobby_character",
    "lobby_ready", "lobby_start", "admin_notice",
}
RESULT_OUTCOMES = ("desync", "disconnected", "error")


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
    accept_timeout: float = 15.0
    udp_timeout: float = 10.0
    punch_timeout: float = 10.0
    ping_report_timeout: float = 15.0
    result_timeout: float = 120.0
    max_match_seconds: float = 3600.0
    idle_timeout: float = 120.0
    tick_seconds: float = 0.5
    online_push_interval: float = 2.0
    token_days: float = 30.0
    login_fail_limit: int = 10
    login_fail_window: float = 300.0
    chat_interval: float = 0.5
    default_rounds: int = 3
    relay: bool = True            # two players who cannot reach each other play through this server's UDP port
    relay_only: bool = False      # development: every match through the relay (one-PC tests of it)
    relay_rate: float = 400.0     # packets a second each player may send through the relay (a game sends ~120)
    relay_burst: float = 800.0
    relay_linger: float = 15.0    # seconds a route outlives its match: the last packets of the end still arrive
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

    def player_for(self, account_id: int | None) -> MatchPlayer | None:
        for p in self.players:
            if p.account_id == account_id:
                return p
        return None

    def other(self, p: MatchPlayer) -> MatchPlayer:
        return self.players[1] if p is self.players[0] else self.players[0]


@dataclass(eq=False)
class LobbyMember:
    session: Session
    character: str = ""
    ready: bool = False
    delay: int = 2


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

    @property
    def state(self) -> str:
        if self.match is not None:
            return "in_match"
        return "full" if len(self.members) >= 2 else "open"

    def member(self, s: Session) -> LobbyMember | None:
        for m in self.members:
            if m.session is s:
                return m
        return None

    def summary(self) -> dict[str, Any]:
        return {
            "id": self.id, "name": self.name, "host": self.host.name, "game": self.game, "mode": self.mode,
            "region": self.region, "players": len(self.members), "max_players": 2,
            "locked": bool(self.password), "state": self.state, "rounds": self.rounds,
        }

    def full_state(self) -> dict[str, Any]:
        return {
            "id": self.id, "name": self.name, "host": self.host.name, "game": self.game, "mode": self.mode,
            "region": self.region, "rounds": self.rounds, "locked": bool(self.password), "state": self.state,
            "match": self.match.id if self.match is not None else None,
            "members": [
                {"name": m.session.name, "character": m.character, "ready": m.ready, "host": m.session is self.host}
                for m in self.members
            ],
            "chat": list(self.chat),
        }


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
        self.lobbies: dict[str, Lobby] = {}
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
        self._ticker_task = asyncio.create_task(self._ticker(), name="mk3d-ticker")
        scheme = "tcp" if self.cfg.plain else "tls"
        log.info("MK3D server %s listening on %s://%s:%d, UDP rendezvous on %d, database %s",
                 P.SERVER_VERSION, scheme, self.cfg.host, self.tcp_port, self.udp_port, self.cfg.db_path)
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
        for ep in [ep for ep, r in self.relay_routes.items() if r.expires is not None and r.expires < now]:
            del self.relay_routes[ep]
        self._run_matchmaking(now)
        self._push_online(now)

    def _prune(self, now: float) -> None:
        window = self.cfg.login_fail_window
        for ip in [ip for ip, q in self.login_failures.items() if not q or q[-1] < now - window]:
            del self.login_failures[ip]
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
        self._detach_account(s)
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
        if not P.version_at_least(version, self.cfg.min_client):
            s.send({**self._error("client_outdated", f"Update the launcher to {self.cfg.min_client} or newer.",
                                  msg), "min_client": self.cfg.min_client})
            s.close()
            return
        s.hello = True
        s.client_version = version
        self._reply(s, msg, {
            "type": "welcome", "proto": P.PROTO, "server": P.SERVER_NAME, "version": P.SERVER_VERSION,
            "udp_port": self.udp_port, "motd": self.cfg.motd, "online": len(self.by_account),
            "min_client": self.cfg.min_client, "games": {g: list(m) for g, m in P.GAMES.items()},
            "regions": list(P.REGIONS),
        })

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
            self._detach_account(old)
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
        self._mark_online()
        log.info("login %s from %s (launcher %s)", s.name, s.ip, s.client_version)

    def _detach_account(self, s: Session) -> None:
        """Take a session out of queue, lobby and pending match, and forget its login."""
        if s.queue is not None:
            self.queue.pop(s.queue.account_id, None)
            s.queue = None
        if s.lobby is not None:
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
        server_rtt = msg.get("server_rtt")
        if isinstance(server_rtt, bool) or not isinstance(server_rtt, int) or not 0 <= server_rtt <= 5000:
            server_rtt = None
        if s.lobby is not None or s.match is not None:
            raise P.ProtocolError("busy", "Leave the lobby or finish the match first.")
        now = time.time()
        rating = self.db.rating(s.account_id, game, mode)["elo"]
        entry = QueueEntry(
            account_id=s.account_id, name=s.name, game=game, mode=mode, ranked=ranked, region=region,
            max_ping=max_ping, strict=strict, character=character, delay=delay, since=now, elo=rating,
            public_ip=s.ip, server_rtt=server_rtt,
        )
        self.queue[s.account_id] = entry
        s.queue = entry
        waiting = sum(1 for e in self.queue.values() if e.key == entry.key)
        self._reply(s, msg, {
            "type": "queued", "game": game, "mode": mode, "ranked": ranked, "region": region,
            "max_ping": max_ping, "strict": strict, "since": now, "in_queue": waiting, "elo": rating,
        })
        log.info("queue %s %s (elo %d, %s, ping<=%s%s)", s.name, entry.queue_name, rating, region,
                 max_ping or "any", ", strict" if strict else "")
        self._mark_online()
        self._run_matchmaking(now)

    def on_queue_leave(self, s: Session, msg: dict[str, Any]) -> None:
        if s.queue is not None:
            self.queue.pop(s.queue.account_id, None)
            s.queue = None
            self._mark_online()
        self._reply(s, msg, {"type": "queue_left", "reason": "left"})

    def _run_matchmaking(self, now: float) -> None:
        if len(self.queue) < 2:
            return
        for a, b in find_pairs(list(self.queue.values()), now, self.cfg.mm, self.ping_cache):
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
                        delay=e.delay, max_ping=e.max_ping, region=e.region, elo=e.elo if e.ranked else None,
                        entry=e)
            for i, (e, sess) in enumerate(pair)
        ]
        m = Match(id=secrets.token_hex(6), game=a.game, mode=a.mode, ranked=a.ranked, players=players, created=now,
                  from_queue=True, rounds=self.cfg.default_rounds, state="found",
                  deadline=now + self.cfg.accept_timeout)
        self._register_match(m)
        for p in players:
            assert p.session is not None
            p.session.send({
                "type": "match_found", "match": m.id, "game": m.game, "mode": m.mode, "ranked": m.ranked,
                "side": p.side, "accept_timeout": self.cfg.accept_timeout, "accept_required": True, "lobby": None,
                "opponent": None if m.ranked else self._opponent(m, m.other(p)),
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
        if self.cfg.relay_only:
            self._start_relay(m, now)
            return
        same_nat = a.session.udp_public[0] == b.session.udp_public[0]
        for p in m.players:
            o = m.other(p)
            assert p.session is not None and o.session is not None
            p.session.send({
                "type": "match_peer", "match": m.id, "side": p.side, "key": m.key,
                "punch_timeout": self.cfg.punch_timeout, "same_nat": same_nat,
                "peer": {"public": P.format_endpoint(o.session.udp_public), "lan": o.session.udp_lan},
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
            })
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
                raise P.ProtocolError("bad_request", "outcome must be desync, disconnected or error.")
            frame = msg.get("frame")
            report: dict[str, Any] = {"outcome": outcome,
                                      "frame": frame if isinstance(frame, int) and not isinstance(frame, bool) else None}
        else:
            report = {
                "winner": P.get_int(msg, "winner", lo=1, hi=2),
                "p1_rounds": P.get_int(msg, "p1_rounds", lo=0, hi=99),
                "p2_rounds": P.get_int(msg, "p2_rounds", lo=0, hi=99),
            }
        now = time.time()
        p.result = report
        self.db.add_report(m.id, p.account_id, p.side, report, now)
        if s.match is m:
            s.match = None  # free to queue again while the other report is outstanding
        self._reply(s, msg, {"type": "result_ack", "match": m.id})
        if all(pl.result is not None for pl in m.players):
            self._finalize(m, now)
        else:
            m.deadline = min(m.deadline, now + self.cfg.result_timeout)

    def _finalize(self, m: Match, now: float) -> None:
        assert self.db is not None
        m.state = "done"
        self.matches.pop(m.id, None)
        self._end_relay(m)
        p1, p2 = m.players
        r1, r2 = p1.result, p2.result
        winner = p1_rounds = p2_rounds = None
        elo_info: dict[str, Any] | None = None
        fields: dict[str, Any] = {"ended": now}
        if r1 is None or r2 is None:
            status = "unconfirmed"
        elif "outcome" in r1 or "outcome" in r2:
            status = "no_contest"
        elif (r1["winner"], r1["p1_rounds"], r1["p2_rounds"]) == (r2["winner"], r2["p1_rounds"], r2["p2_rounds"]):
            status = "agreed"
            winner, p1_rounds, p2_rounds = r1["winner"], r1["p1_rounds"], r1["p2_rounds"]
        else:
            status = "disputed"
        if status == "agreed":
            w, lose = (p1, p2) if winner == 1 else (p2, p1)
            if m.ranked:
                changes = self.db.record_ranked(w.account_id, lose.account_id, m.game, m.mode, now)
                elo_info = {}
                for p, prefix in ((p1, "p1"), (p2, "p2")):
                    before, after = changes[p.account_id]
                    elo_info[p.name] = {"before": before, "after": after, "delta": after - before}
                    fields[f"{prefix}_elo_before"] = before
                    fields[f"{prefix}_elo_after"] = after
            else:
                self.db.record_casual(w.account_id, lose.account_id, m.game, m.mode, now)
            fields.update(winner=winner, p1_rounds=p1_rounds, p2_rounds=p2_rounds)
            log.info("RESULT match=%s %s beat %s %s-%s (%s)", m.id, w.name, lose.name, p1_rounds, p2_rounds,
                     "ranked" if m.ranked else "casual")
        elif status == "disputed":
            log.warning("DISPUTE match=%s %s reported %s, %s reported %s; no rating change",
                        m.id, p1.name, r1, p2.name, r2)
        else:
            log.info("%s match=%s %s=%s %s=%s; no rating change", status.upper(), m.id, p1.name, r1, p2.name, r2)
        fields["status"] = status
        self.db.update_match(m.id, **fields)
        out: dict[str, Any] = {"type": "match_result", "match": m.id, "status": status, "winner": winner,
                               "p1_rounds": p1_rounds, "p2_rounds": p2_rounds, "ranked": m.ranked}
        if elo_info is not None:
            out["elo"] = elo_info
        for p in m.players:
            sess = self.by_account.get(p.account_id)
            if sess is not None:
                if sess.match is m:
                    sess.match = None
                sess.send(dict(out))
        if m.lobby is not None:
            self._lobby_match_over(m.lobby, m)
        self._mark_online()

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

    def _need_lobby(self, s: Session) -> Lobby:
        if s.lobby is None:
            raise P.ProtocolError("not_in_lobby", "You are not in a lobby.")
        return s.lobby

    def on_lobby_create(self, s: Session, msg: dict[str, Any]) -> None:
        if s.queue is not None or s.lobby is not None or s.match is not None:
            raise P.ProtocolError("busy", "Leave the queue, lobby or match first.")
        game, mode = P.get_game_mode(msg)
        default_name = f"{s.name}'s lobby"
        name = P.get_str(msg, "name", default=default_name, max_len=128).strip()[: P.LOBBY_NAME_MAX] or default_name
        password = P.get_str(msg, "password", default="", max_len=64)
        rounds = P.get_int(msg, "rounds", lo=1, hi=9, default=self.cfg.default_rounds)
        if rounds not in P.ROUNDS_ALLOWED:
            raise P.ProtocolError("bad_request", "rounds must be 1, 3 or 5.")
        region = P.get_region(msg)
        character = P.get_character(msg)
        lobby = Lobby(id=self._new_lobby_code(), name=name, game=game, mode=mode, region=region, rounds=rounds,
                      password=password, host=s, members=[LobbyMember(s, character)], created=time.time())
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
        if lobby.match is not None or len(lobby.members) >= 2:
            raise P.ProtocolError("lobby_full", "That lobby is full.")
        password = P.get_str(msg, "password", default="", max_len=64)
        if lobby.password and not hmac.compare_digest(lobby.password.encode(), password.encode()):
            raise P.ProtocolError("bad_lobby_password", "Wrong lobby password.")
        lobby.members.append(LobbyMember(s, P.get_character(msg)))
        s.lobby = lobby
        log.info("lobby %s: %s joined", lobby.id, s.name)
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def _leave_lobby(self, s: Session) -> None:
        lobby = s.lobby
        if lobby is None:
            return
        s.lobby = None
        member = lobby.member(s)
        if member is not None:
            lobby.members.remove(member)
        if not lobby.members:
            self.lobbies.pop(lobby.id, None)
        elif lobby.host is s:
            lobby.host = lobby.members[0].session
        for other in lobby.members:
            other.ready = False
        log.info("lobby %s: %s left", lobby.id, s.name)
        m = lobby.match
        if m is not None and m.state in PENDING_STATES:
            p = m.player_for(s.account_id)
            self._cancel_match(m, "peer_left", blame={p.side} if p else set())  # broadcasts the lobby
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
        member.ready = P.get_bool(msg, "ready")
        member.delay = P.get_int(msg, "delay", lo=0, hi=P.DELAY_MAX, default=member.delay)
        self._broadcast_lobby(lobby, reply_to=(s, msg))

    def on_lobby_start(self, s: Session, msg: dict[str, Any]) -> None:
        lobby = self._need_lobby(s)
        if lobby.host is not s:
            raise P.ProtocolError("not_host", "Only the host can start.")
        if lobby.match is not None:
            raise P.ProtocolError("busy", "A match is already running.")
        if len(lobby.members) < 2 or not all(m.ready for m in lobby.members):
            raise P.ProtocolError("not_ready", "Both players must be ready.")
        now = time.time()
        ordered = sorted(lobby.members, key=lambda mem: mem.session is not lobby.host)  # host first
        players = [
            MatchPlayer(side=i + 1, account_id=mem.session.account_id or 0, name=mem.session.name or "?",
                        session=mem.session, character=mem.character, delay=mem.delay, max_ping=0,
                        region=lobby.region, elo=None, accepted=True)
            for i, mem in enumerate(ordered)
        ]
        m = Match(id=secrets.token_hex(6), game=lobby.game, mode=lobby.mode, ranked=False, players=players,
                  created=now, from_queue=False, lobby=lobby, rounds=lobby.rounds, state="accepted",
                  deadline=now + self.cfg.udp_timeout)
        lobby.match = m
        self._register_match(m)
        for p in players:
            assert p.session is not None
            p.session.send({
                "type": "match_found", "match": m.id, "game": m.game, "mode": m.mode, "ranked": False,
                "side": p.side, "accept_timeout": 0, "accept_required": False, "lobby": lobby.id,
                "opponent": self._opponent(m, m.other(p)),
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
