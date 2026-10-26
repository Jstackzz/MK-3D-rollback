#!/usr/bin/env python3
"""Development stand-in for a game executable: speaks the launcher <-> game file protocol
(docs/PROTOCOL.md section 3) without any game, so the launcher can be tested end to end.

Point a game's "Executable" in the launcher's Settings at this file (the launcher runs .py files
with Python). It reads MK3D_STATUS and MK3D_CMD from the environment, like the real game.

    booting -> ready
    start (online)        binds local_port, exchanges heartbeats with remote, writes in_match;
                          after MK3D_FAKE_SECONDS (default 6) writes a result derived from the seed,
                          so both sides of a match always agree; disconnected if the peer goes quiet,
                          "error no connection" if it never answers
    start mode=practice   in_match match=practice until park
    park                  back to ready
    rematch [seed=N]      another match with the same peer
    quit                  exit

Other knobs: MK3D_FAKE_BOOT (seconds before "ready", default 1.5), MK3D_FAKE_RESULT=desync to
report "desync frame=600" instead of a result.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import time


def simulated_result(seed: int, rounds: int = 3) -> tuple[int, int, int]:
    """(winner, p1_rounds, p2_rounds) from the seed, the same rule as server/client.py."""
    need = rounds // 2 + 1
    winner = 1 if seed % 2 == 0 else 2
    loser_rounds = (seed >> 1) % need
    return (winner, need, loser_rounds) if winner == 1 else (winner, loser_rounds, need)


def log(text: str) -> None:
    print(f"[fake_game {os.environ.get('MK3D_INSTANCE', '1')}] {text}", flush=True)


def write_atomic(path: str, text: str) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text + "\n")
    for _ in range(200):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.005)
    log(f"could not replace {path}")


def take_command(path: str) -> str | None:
    taken = path + ".taken"
    try:
        os.replace(path, taken)
    except FileNotFoundError:
        return None
    except PermissionError:
        return None
    with open(taken, encoding="ascii", errors="replace") as fh:
        return fh.readline().strip()


def parse(line: str) -> tuple[str, dict[str, str]]:
    verb, *rest = line.split()
    keys = {}
    for token in rest:
        if "=" in token:
            k, _, v = token.partition("=")
            keys[k] = v
    return verb, keys


class FakeGame:
    def __init__(self, status_path: str, cmd_path: str) -> None:
        self.status_path = status_path
        self.cmd_path = cmd_path
        self.match_seconds = float(os.environ.get("MK3D_FAKE_SECONDS", "6"))
        self.sock: socket.socket | None = None
        self.state = "booting"
        self.start: dict[str, str] = {}
        self.match = ""
        self.started_at = 0.0
        self.connected_at = 0.0
        self.last_peer = 0.0
        self.last_send = 0.0
        self.sent = 0

    def status(self, line: str) -> None:
        log(f"status: {line}")
        write_atomic(self.status_path, line)

    def close_socket(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def begin(self, keys: dict[str, str]) -> None:
        self.close_socket()
        self.start = keys
        self.match = keys.get("match", "match")
        self.started_at = time.monotonic()
        self.connected_at = 0.0
        self.last_peer = 0.0
        if keys.get("mode") == "practice":
            self.state = "practice"
            self.status("in_match match=practice")
            return
        port = int(keys.get("local_port", "7001"))
        host, _, rport = keys.get("remote", "127.0.0.1:7002").partition(":")
        self.remote = (host, int(rport))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # a loopback peer needs no public socket (and then Windows asks nothing about the firewall)
        self.sock.bind(("127.0.0.1" if host.startswith("127.") else "0.0.0.0", port))
        self.sock.setblocking(False)
        self.state = "connecting"
        log(f"side {keys.get('side')} on udp {port} -> {self.remote}, seed {keys.get('seed')}")

    def handle(self, line: str) -> bool:
        log(f"cmd: {line}")
        verb, keys = parse(line)
        if verb == "quit":
            self.close_socket()
            return False
        if verb == "park":
            self.close_socket()
            self.state = "ready"
            self.status("ready")
        elif verb == "start":
            self.begin(keys)
        elif verb == "rematch" and self.start:
            again = dict(self.start)
            again.update(keys)
            self.begin(again)
        else:
            self.status(f"error bad command {verb}")
        return True

    def pump_network(self, now: float) -> None:
        if self.sock is None:
            return
        if now - self.last_send >= 0.1:
            self.sent += 1
            hb = {"type": "hb", "match": self.match, "side": int(self.start.get("side", "1")), "n": self.sent}
            try:
                self.sock.sendto(json.dumps(hb).encode(), self.remote)
            except OSError:
                pass
            self.last_send = now
        while True:
            try:
                data, _ = self.sock.recvfrom(2048)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if msg.get("type") == "hb" and msg.get("match") == self.match:
                self.last_peer = now

    def step(self, now: float) -> None:
        if self.state == "connecting":
            self.pump_network(now)
            if self.last_peer:
                self.state = "playing"
                self.connected_at = now
                self.status(f"in_match match={self.match}")
            elif now - self.started_at > 10:
                self.close_socket()
                self.state = "over"
                self.status("error no connection")
        elif self.state == "playing":
            self.pump_network(now)
            if now - self.last_peer > 3:
                self.close_socket()
                self.state = "over"
                self.status(f"disconnected match={self.match}")
            elif now - self.connected_at >= self.match_seconds:
                self.state = "over"
                if os.environ.get("MK3D_FAKE_RESULT") == "desync":
                    self.status(f"desync frame=600 match={self.match}")
                else:
                    seed = int(self.start.get("seed", "0"))
                    winner, r1, r2 = simulated_result(seed, int(self.start.get("rounds", "3")))
                    self.status(f"result winner={winner} p1_rounds={r1} p2_rounds={r2} match={self.match}")
        elif self.state == "over":
            self.pump_network(now)  # keep answering the peer until park

    def run(self) -> int:
        self.status("booting")
        time.sleep(float(os.environ.get("MK3D_FAKE_BOOT", "1.5")))
        self.state = "ready"
        self.status("ready")
        while True:
            line = take_command(self.cmd_path)
            if line and not self.handle(line):
                log("bye")
                return 0
            self.step(time.monotonic())
            time.sleep(0.02)


def main() -> int:
    status_path, cmd_path = os.environ.get("MK3D_STATUS"), os.environ.get("MK3D_CMD")
    if not status_path or not cmd_path:
        print("run me from the MK3D launcher (MK3D_STATUS and MK3D_CMD are not set)", file=sys.stderr)
        return 2
    return FakeGame(status_path, cmd_path).run()


if __name__ == "__main__":
    sys.exit(main())
