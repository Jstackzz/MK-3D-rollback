"""python -m server --host 0.0.0.0 --port 7777 --udp-port 7778 --db server.sqlite"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys

from . import __version__
from .app import Server, ServerConfig
from .matchmaking import MatchmakingConfig


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="python -m server", description="MK3D Online lobby and matchmaking server.")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=7777, help="TLS/TCP port (default 7777)")
    ap.add_argument("--udp-port", type=int, default=7778, help="UDP rendezvous port (default 7778)")
    ap.add_argument("--db", default="server.sqlite", help="SQLite database file (default server.sqlite)")
    ap.add_argument("--cert", default=None, help="certificate PEM (default: server-cert.pem next to the database)")
    ap.add_argument("--key", default=None, help="private key PEM (default: server-key.pem next to the database)")
    ap.add_argument("--plain", action="store_true", help="development only: plain TCP without TLS")
    ap.add_argument("--motd", default="", help="message of the day sent in welcome")
    ap.add_argument("--admins", default="", help="comma-separated account names allowed to send notices")
    ap.add_argument("--min-client", default="0.1.0", help="oldest launcher version accepted (default 0.1.0)")
    ap.add_argument("--update-file", default=None,
                    help="the release launchers are offered to update to (default: update.json next to the database)")
    ap.add_argument("--allow-same-ip", action="store_true",
                    help="let ranked pair two players from the same public IP (LAN events, development)")
    ap.add_argument("--accept-timeout", type=float, default=20.0, help="seconds to accept a found match")
    ap.add_argument("--no-relay", action="store_true",
                    help="cancel a match whose players cannot reach each other instead of relaying it")
    ap.add_argument("--upnp", action="store_true",
                    help="open the TCP and UDP ports on this network's router (UPnP): a server on a home PC")
    ap.add_argument("--relay-only", action="store_true",
                    help="relay every match through this server: no player ever learns another's IP address, but "
                         "players far from the server get much more ping (by default only the matches of players "
                         "who ask to hide their address are relayed)")
    ap.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    ap.add_argument("--log-file", default=None, help="also append the log to this file")
    ap.add_argument("--version", action="version", version=f"mk3d-server {__version__}")
    return ap


def config_from_args(args: argparse.Namespace) -> ServerConfig:
    base = os.path.dirname(os.path.abspath(args.db))
    return ServerConfig(
        host=args.host,
        port=args.port,
        udp_port=args.udp_port,
        db_path=args.db,
        certfile=args.cert or os.path.join(base, "server-cert.pem"),
        keyfile=args.key or os.path.join(base, "server-key.pem"),
        plain=args.plain,
        motd=args.motd,
        admins=tuple(a.strip() for a in args.admins.split(",") if a.strip()),
        min_client=args.min_client,
        update_file=args.update_file or os.path.join(base, "update.json"),
        accept_timeout=args.accept_timeout,
        relay=not args.no_relay,
        upnp=args.upnp,
        relay_only=args.relay_only,
        mm=MatchmakingConfig(allow_same_ip=args.allow_same_ip),
    )


async def serve(cfg: ServerConfig) -> None:
    server = Server(cfg)
    await server.start()
    try:
        await asyncio.Event().wait()
    finally:
        await server.stop()


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stderr)]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding="utf-8"))
    logging.basicConfig(level=getattr(logging, args.log_level), handlers=handlers,
                        format="%(asctime)s %(levelname)-7s %(message)s")
    cfg = config_from_args(args)
    if not cfg.plain and not (os.path.exists(cfg.certfile or "") and os.path.exists(cfg.keyfile or "")):
        print(f"No certificate at {cfg.certfile}.\nCreate one with:  python -m server.makecert --out-dir "
              f"{os.path.dirname(cfg.certfile or '.') or '.'}\n(or pass --cert/--key, or --plain for local tests)",
              file=sys.stderr)
        return 2
    try:
        asyncio.run(serve(cfg))
    except KeyboardInterrupt:
        logging.getLogger("mk3d.server").info("stopped")
    except OSError as exc:
        print(f"cannot start: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
