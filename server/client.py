"""A small asyncio client for the MK3D server, used by the tests and as a scripted second player.

    python -m server.client demo --host 127.0.0.1 --port 7777 --fingerprint <sha256>
        two throw-away accounts queue for casual mkd kombat and play one simulated match

    python -m server.client play --host 127.0.0.1 --port 7777 --fingerprint <sha256> --name tester2
        [--register] [--game mkd --mode kombat] [--ranked] [--region eu] [--character Raiden] [--udp-port 7002]
        one scripted player: queue, accept, punch, report the simulated result

    python -m server.client notice --host lobby.example.org --fingerprint <sha256> --name admin --text "..."
        send a server notice to everyone online (the account must be listed in the server's --admins)

The password for `play` and `notice` is read from the environment variable MK3D_PASSWORD (never from the
command line); without it a random one is generated (fine for --register of a throw-away account).
The simulated result is a function of the match seed, so two scripted players (or a scripted
player and launcher/tools/fake_game.py) always agree.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import itertools
import json
import os
import secrets
import ssl
import sys
import time
from collections.abc import Callable
from typing import Any

from . import protocol as P
from .makecert import normalize_fingerprint

CLIENT_NAME = "mk3d-testclient"
CLIENT_VERSION = "0.1.0"


class ServerError(Exception):
    def __init__(self, msg: dict[str, Any]) -> None:
        super().__init__(f"{msg.get('code')}: {msg.get('message')}")
        self.code = msg.get("code")
        self.msg = msg


class FingerprintMismatch(Exception):
    pass


def simulated_result(seed: int, rounds: int = 3) -> tuple[int, int, int]:
    """(winner, p1_rounds, p2_rounds) derived from the seed; identical on both sides."""
    need = rounds // 2 + 1
    winner = 1 if seed % 2 == 0 else 2
    loser_rounds = (seed >> 1) % need
    return (winner, need, loser_rounds) if winner == 1 else (winner, loser_rounds, need)


class Connection:
    """One NDJSON connection; incoming messages are kept in `inbox` until a test takes them."""

    def __init__(self, host: str, port: int, *, fingerprint: str | None = None, plain: bool = False) -> None:
        self.host, self.port = host, port
        self.fingerprint = normalize_fingerprint(fingerprint) if fingerprint else None
        self.plain = plain
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self.inbox: list[dict[str, Any]] = []
        self.closed = False
        self.welcome: dict[str, Any] = {}
        self.account: dict[str, Any] = {}
        self.peer_fingerprint = ""
        self._cond = asyncio.Condition()
        self._ids = itertools.count(1)
        self._task: asyncio.Task | None = None
        self._keepalive: asyncio.Task | None = None

    async def connect(self, *, hello: bool = True, version: str = CLIENT_VERSION) -> dict[str, Any]:
        if self.plain:
            self.reader, self.writer = await asyncio.open_connection(self.host, self.port, limit=P.MAX_LINE + 2)
        else:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE  # trust comes from the pinned fingerprint below
            self.reader, self.writer = await asyncio.open_connection(
                self.host, self.port, ssl=ctx, server_hostname=self.host, limit=P.MAX_LINE + 2
            )
            der = self.writer.get_extra_info("ssl_object").getpeercert(binary_form=True)
            self.peer_fingerprint = hashlib.sha256(der).hexdigest()
            if self.fingerprint and self.peer_fingerprint != self.fingerprint:
                self.writer.close()
                raise FingerprintMismatch(f"server certificate {self.peer_fingerprint} is not the pinned one")
        self._task = asyncio.create_task(self._read_loop())
        self._keepalive = asyncio.create_task(self._ping_loop())
        if hello:
            self.welcome = await self.request({"type": "hello", "proto": P.PROTO, "client": CLIENT_NAME,
                                               "version": version, "os": sys.platform})
        return self.welcome

    async def _read_loop(self) -> None:
        assert self.reader is not None
        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                async with self._cond:
                    self.inbox.append(msg)
                    self._cond.notify_all()
        except (ConnectionError, OSError, ssl.SSLError, asyncio.IncompleteReadError):
            pass
        finally:
            self.closed = True
            async with self._cond:
                self._cond.notify_all()

    async def _ping_loop(self) -> None:
        """Like the launcher: a ping every 30 s keeps the server's 120 s idle timer from firing."""
        while not self.closed:
            await asyncio.sleep(30)
            if not self.closed and self.welcome:
                self.send({"type": "ping"})

    def send(self, msg: dict[str, Any]) -> None:
        assert self.writer is not None
        self.writer.write(P.encode(msg))

    async def expect(self, mtype: str | None = None, *, timeout: float = 5.0,
                     pred: Callable[[dict[str, Any]], bool] | None = None, **fields: Any) -> dict[str, Any]:
        """Take the first inbox message of `mtype` (and matching fields/pred); wait for it if needed."""

        def matches(m: dict[str, Any]) -> bool:
            if mtype is not None and m.get("type") != mtype:
                return False
            if any(m.get(k) != v for k, v in fields.items()):
                return False
            return pred(m) if pred is not None else True

        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        async with self._cond:
            while True:
                for i, m in enumerate(self.inbox):
                    if matches(m):
                        del self.inbox[i]
                        return m
                if self.closed:
                    raise ConnectionError(f"connection closed while waiting for {mtype}")
                remaining = deadline - loop.time()
                if remaining <= 0:
                    raise TimeoutError(f"no {mtype} within {timeout}s; inbox has {[m.get('type') for m in self.inbox]}")
                try:
                    await asyncio.wait_for(self._cond.wait(), remaining)
                except TimeoutError:
                    pass

    def take_all(self, mtype: str) -> list[dict[str, Any]]:
        out = [m for m in self.inbox if m.get("type") == mtype]
        self.inbox = [m for m in self.inbox if m.get("type") != mtype]
        return out

    async def request(self, msg: dict[str, Any], *, timeout: float = 10.0) -> dict[str, Any]:
        """Send with a fresh id and return the reply carrying that id; `error` replies raise ServerError."""
        rid = next(self._ids)
        self.send({**msg, "id": rid})
        reply = await self.expect(timeout=timeout, pred=lambda m: m.get("id") == rid)
        if reply.get("type") == "error":
            raise ServerError(reply)
        return reply

    async def register(self, name: str, password: str) -> dict[str, Any]:
        self.account = await self.request({"type": "register", "name": name, "password": password})
        return self.account

    async def login(self, name: str, password: str) -> dict[str, Any]:
        self.account = await self.request({"type": "login", "name": name, "password": password})
        return self.account

    async def close(self) -> None:
        if self.writer is not None:
            self.writer.close()
            try:
                await asyncio.wait_for(self.writer.wait_closed(), timeout=2)
            except (Exception, asyncio.CancelledError):
                pass
        for task in (self._task, self._keepalive):
            if task is not None:
                task.cancel()
                try:
                    await task
                except (Exception, asyncio.CancelledError):
                    pass


class UdpPeer(asyncio.DatagramProtocol):
    """The launcher's UDP side: hello to the rendezvous, then punch and ping the peer."""

    def __init__(self) -> None:
        self.transport: asyncio.DatagramTransport | None = None
        self.queue: asyncio.Queue[tuple[dict[str, Any], tuple[str, int]]] = asyncio.Queue()
        self.raw: asyncio.Queue[tuple[bytes, tuple[str, int]]] = asyncio.Queue()  # datagrams that are not JSON
        self.match = ""
        self.key = ""
        self.side = 0

    @classmethod
    async def open(cls, host: str = "127.0.0.1", port: int = 0) -> UdpPeer:
        loop = asyncio.get_running_loop()
        _, proto = await loop.create_datagram_endpoint(cls, local_addr=(host, port))
        return proto

    @property
    def port(self) -> int:
        assert self.transport is not None
        return self.transport.get_extra_info("sockname")[1]

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def error_received(self, exc: Exception) -> None:
        pass

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            msg = json.loads(data)
        except ValueError:
            self.raw.put_nowait((data, (addr[0], addr[1])))
            return
        if not isinstance(msg, dict):
            return
        kind = msg.get("type")
        if kind in ("punch", "ping") and self.key and msg.get("match") == self.match and msg.get("key") == self.key:
            self.sendto({**msg, "type": "punch_ack" if kind == "punch" else "pong", "side": self.side}, addr)
        elif kind == "hb" and self.match and msg.get("match") == self.match:
            # heartbeat of launcher/tools/fake_game.py: answer so it sees a live peer
            self.sendto({"type": "hb", "match": self.match, "side": self.side}, addr)
        self.queue.put_nowait((msg, (addr[0], addr[1])))

    def sendto(self, msg: dict[str, Any], addr: tuple[str, int]) -> None:
        if self.transport is not None:
            self.transport.sendto(P.encode_datagram(msg), addr)

    def close(self) -> None:
        if self.transport is not None:
            self.transport.close()
            self.transport = None

    async def recv(self, timeout: float, pred: Callable[[dict[str, Any], tuple[str, int]], bool]
                   ) -> tuple[dict[str, Any], tuple[str, int]] | None:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                return None
            try:
                msg, addr = await asyncio.wait_for(self.queue.get(), remaining)
            except TimeoutError:
                return None
            if pred(msg, addr):
                return msg, addr

    async def hello(self, server: tuple[str, int], token: str, *, match: str = "", lan: str = "",
                    timeout: float = 10.0) -> dict[str, Any] | None:
        """Send hellos every 250 ms until hello_ack; returns it with `rtt_ms` added."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            t = int(loop.time() * 1000)
            self.sendto({"type": "hello", "token": token, "match": match, "lan": lan, "t": t}, server)
            got = await self.recv(0.25, lambda m, a: m.get("type") == "hello_ack")
            if got is not None:
                ack = got[0]
                ack["rtt_ms"] = max(0, int(loop.time() * 1000) - int(ack.get("t", t)))
                return ack
        return None

    async def punch(self, peer: dict[str, str], *, match: str, key: str, side: int, timeout: float = 10.0,
                    pings: int = 5) -> dict[str, Any]:
        """Punch both candidates, then measure; returns the ping_report fields."""
        self.match, self.key, self.side = match, key, side
        candidates: list[tuple[str, tuple[str, int]]] = []
        for via in ("lan", "public"):
            ep = P.parse_endpoint(peer.get(via))
            if ep is not None and ep not in [c[1] for c in candidates]:
                candidates.append((via, ep))
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        working: tuple[str, tuple[str, int]] | None = None
        while working is None and candidates and loop.time() < deadline:
            for _, ep in candidates:
                self.sendto({"type": "punch", "match": match, "key": key, "side": side}, ep)
            got = await self.recv(0.1, lambda m, a: m.get("type") == "punch_ack" and m.get("match") == match
                                  and m.get("key") == key)
            if got is not None:
                addr = got[1]
                working = (next((v for v, ep in candidates if ep == addr), "public"), addr)
        if working is None:
            return {"ok": False, "rtt_ms": 0}
        rtts: list[float] = []
        for seq in range(pings):
            t = loop.time() * 1000
            self.sendto({"type": "ping", "match": match, "key": key, "seq": seq, "t": int(t)}, working[1])
            got = await self.recv(0.5, lambda m, a, s=seq: m.get("type") == "pong" and m.get("seq") == s
                                  and m.get("key") == key)
            if got is not None:
                rtts.append(loop.time() * 1000 - t)
            await asyncio.sleep(0.1)
        if len(rtts) < 3:
            return {"ok": False, "rtt_ms": 0}
        rtts.sort()
        return {"ok": True, "rtt_ms": int(round(rtts[len(rtts) // 2])), "remote": P.format_endpoint(working[1]),
                "via": working[0]}


class TunnelBridge(asyncio.DatagramProtocol):
    """The launcher's way when its UDP never reaches the server (udp_tunnel): a local port stands in for the relay.
    What the game port sends to it goes on the connection as `tunnel` messages, and what comes back that way is sent
    to the game port, from here: the game port's punch and its game take this port for the relay's."""

    def __init__(self, conn: Connection, game_port: int) -> None:
        self.conn = conn
        self.game = ("127.0.0.1", game_port)
        self.transport: asyncio.DatagramTransport | None = None
        self._task: asyncio.Task | None = None

    @classmethod
    async def open(cls, conn: Connection, game_port: int) -> TunnelBridge:
        loop = asyncio.get_running_loop()
        _, bridge = await loop.create_datagram_endpoint(lambda: cls(conn, game_port), local_addr=("127.0.0.1", 0))
        bridge._task = asyncio.create_task(bridge._down())
        return bridge

    @property
    def endpoint(self) -> str:
        assert self.transport is not None
        host, port = self.transport.get_extra_info("sockname")[:2]
        return f"{host}:{port}"

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def error_received(self, exc: Exception) -> None:
        pass

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        if (addr[0], addr[1]) == self.game:
            self.conn.send({"type": "tunnel", "data": base64.b64encode(data).decode("ascii")})

    async def _down(self) -> None:
        while True:
            try:
                msg = await self.conn.expect("tunnel", timeout=3600)
            except (ConnectionError, TimeoutError):
                return
            if self.transport is not None:
                self.transport.sendto(base64.b64decode(msg["data"]), self.game)

    def close(self) -> None:
        if self._task is not None:
            self._task.cancel()
        if self.transport is not None:
            self.transport.close()
            self.transport = None


async def play_one_match(conn: Connection, udp: UdpPeer, *, server_host: str, lan_ip: str = "127.0.0.1",
                         accept: bool = True, game_seconds: float = 1.0, say: Callable[[str], None] = print,
                         result_override: tuple[int, int, int] | None = None) -> dict[str, Any]:
    """Drive one match from match_found to match_result like a launcher with a simulated game."""
    server_udp = (server_host, int(conn.welcome["udp_port"]))
    token = conn.account["udp_token"]
    found = await conn.expect("match_found", timeout=600)
    opp = found.get("opponent") or {}
    say(f"match_found {found['match']} {found['game']}/{found['mode']} {'ranked' if found['ranked'] else 'casual'} "
        f"side={found['side']} opponent={opp.get('name', 'hidden')}")
    ack = await udp.hello(server_udp, token, match=found["match"], lan=f"{lan_ip}:{udp.port}")
    say(f"hello_ack public={ack.get('public') if ack else None}")
    if found.get("accept_required"):
        conn.send({"type": "match_accept" if accept else "match_decline", "match": found["match"]})
        say("accepted" if accept else "declined")
        if not accept:
            return await conn.expect("match_cancelled", timeout=30)

    def peer_or_cancel(m: dict[str, Any]) -> bool:
        return m.get("type") in ("match_peer", "match_cancelled") and m.get("match") == found["match"]

    peer = await conn.expect(pred=peer_or_cancel, timeout=60)
    if peer["type"] == "match_cancelled":
        say(f"match_cancelled {peer['reason']}")
        return peer
    say(f"match_peer peer={peer['peer']} same_nat={peer['same_nat']}")
    report = await udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"],
                             timeout=float(peer.get("punch_timeout", 10)))
    conn.send({"type": "ping_report", "match": peer["match"], **report})
    say(f"ping_report {report}")

    def go_or_cancel(m: dict[str, Any]) -> bool:
        return m.get("type") in ("match_go", "match_cancelled") and m.get("match") == found["match"]

    go = await conn.expect(pred=go_or_cancel, timeout=60)
    if go["type"] == "match_cancelled":
        say(f"match_cancelled {go['reason']}")
        return go
    say(f"match_go side={go['side']} remote={go['remote']} rtt={go['rtt_ms']}ms delay={go['delay']} "
        f"seed={go['seed']} p1={go['p1']} p2={go['p2']}")
    await asyncio.sleep(game_seconds)  # the "game"
    winner, r1, r2 = result_override or simulated_result(int(go["seed"]), int(go["rounds"]))
    await conn.request({"type": "result", "match": go["match"], "winner": winner, "p1_rounds": r1, "p2_rounds": r2})
    say(f"result winner={winner} p1_rounds={r1} p2_rounds={r2}")
    final = await conn.expect("match_result", timeout=180, match=go["match"])
    say(f"match_result {final['status']} elo={final.get('elo')}")
    return final


# --- command line -------------------------------------------------------------------------------


async def _login_or_register(conn: Connection, name: str, password: str, register: bool) -> None:
    if register:
        try:
            await conn.register(name, password)
            return
        except ServerError as exc:
            if exc.code != "name_taken":
                raise
    await conn.login(name, password)


async def cmd_play(args: argparse.Namespace) -> int:
    password = os.environ.get("MK3D_PASSWORD") or secrets.token_urlsafe(12)
    conn = Connection(args.host, args.port, fingerprint=args.fingerprint, plain=args.plain)
    udp = await UdpPeer.open("127.0.0.1" if args.host.startswith("127.") else "0.0.0.0", args.udp_port)
    try:
        await conn.connect()
        await _login_or_register(conn, args.name, password, args.register)
        print(f"[{args.name}] logged in; udp port {udp.port}", flush=True)
        ack = await udp.hello((args.host, int(conn.welcome["udp_port"])), conn.account["udp_token"],
                              lan=f"{args.lan_ip}:{udp.port}", timeout=3)
        await conn.request({"type": "queue_join", "game": args.game, "mode": args.mode, "ranked": args.ranked,
                            "region": args.region, "max_ping": args.max_ping, "strict": False,
                            "character": args.character, "delay": 2,
                            **({"server_rtt": ack["rtt_ms"]} if ack else {})})
        print(f"[{args.name}] queued for {args.game}/{args.mode} {'ranked' if args.ranked else 'casual'}", flush=True)
        for _ in range(args.matches):
            final = await play_one_match(conn, udp, server_host=args.host, lan_ip=args.lan_ip,
                                         game_seconds=args.game_seconds,
                                         say=lambda t: print(f"[{args.name}] {t}", flush=True))
            if final.get("type") != "match_result":
                return 1
        return 0
    finally:
        udp.close()
        await conn.close()


async def cmd_notice(args: argparse.Namespace) -> int:
    password = os.environ.get("MK3D_PASSWORD")
    if not password:
        print("set MK3D_PASSWORD to the admin account's password", file=sys.stderr)
        return 2
    conn = Connection(args.host, args.port, fingerprint=args.fingerprint, plain=args.plain)
    try:
        await conn.connect()
        await conn.login(args.name, password)
        conn.send({"type": "admin_notice", "text": args.text, "level": args.level, "id": "notice"})
        reply = await conn.expect(pred=lambda m: m.get("type") == "notice" or m.get("id") == "notice", timeout=10)
        if reply.get("type") == "error":
            raise ServerError(reply)
        print(f"notice sent: {reply['text']}")
        return 0
    finally:
        await conn.close()


async def cmd_demo(args: argparse.Namespace) -> int:
    tag = secrets.token_hex(3)
    names = [f"demo{tag}a", f"demo{tag}b"]
    conns, udps = [], []
    try:
        for name in names:
            conn = Connection(args.host, args.port, fingerprint=args.fingerprint, plain=args.plain)
            await conn.connect()
            await conn.register(name, secrets.token_urlsafe(12))
            conns.append(conn)
            udps.append(await UdpPeer.open("127.0.0.1", 0))
            print(f"[{name}] registered", flush=True)
        for name, conn in zip(names, conns):
            await conn.request({"type": "queue_join", "game": args.game, "mode": args.mode, "ranked": False,
                                "region": "eu", "max_ping": 0, "strict": False, "character": "Scorpion",
                                "delay": 2})
            print(f"[{name}] queued for {args.game}/{args.mode} casual", flush=True)
        results = await asyncio.gather(*[
            play_one_match(conn, udp, server_host=args.host, game_seconds=0.5,
                           say=lambda t, n=name: print(f"[{n}] {t}", flush=True))
            for name, conn, udp in zip(names, conns, udps)
        ])
        ok = all(r.get("type") == "match_result" and r.get("status") == "agreed" for r in results)
        print("demo OK" if ok else "demo FAILED", flush=True)
        return 0 if ok else 1
    finally:
        for udp in udps:
            udp.close()
        for conn in conns:
            await conn.close()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m server.client", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("play", "demo", "notice"):
        sp = sub.add_parser(name)
        sp.add_argument("--host", default="127.0.0.1")
        sp.add_argument("--port", type=int, default=7777)
        sp.add_argument("--fingerprint", default=None, help="pinned SHA-256 of the server certificate")
        sp.add_argument("--plain", action="store_true", help="server runs with --plain")
        sp.add_argument("--game", default="mkd")
        sp.add_argument("--mode", default="kombat")
    play = sub.choices["play"]
    play.add_argument("--name", required=True)
    play.add_argument("--register", action="store_true", help="create the account if it does not exist")
    play.add_argument("--ranked", action="store_true")
    play.add_argument("--region", default="eu")
    play.add_argument("--max-ping", type=int, default=0)
    play.add_argument("--character", default="Raiden")
    play.add_argument("--udp-port", type=int, default=0, help="local UDP port (0 = any)")
    play.add_argument("--lan-ip", default="127.0.0.1", help="LAN address announced in the hello")
    play.add_argument("--matches", type=int, default=1)
    play.add_argument("--game-seconds", type=float, default=2.0)
    notice = sub.choices["notice"]
    notice.add_argument("--name", required=True, help="an account listed in the server's --admins")
    notice.add_argument("--text", required=True)
    notice.add_argument("--level", choices=["info", "warn"], default="info")
    args = ap.parse_args(argv)
    t0 = time.time()
    commands = {"play": cmd_play, "demo": cmd_demo, "notice": cmd_notice}
    try:
        rc = asyncio.run(commands[args.cmd](args))
    except (ServerError, FingerprintMismatch, ConnectionError, TimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        rc = 1
    print(f"done in {time.time() - t0:.1f}s", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
