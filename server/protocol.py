"""Constants, message encoding and field validation shared by the server and the test client."""

from __future__ import annotations

import ipaddress
import json
import re
from typing import Any

from . import __version__

PROTO = 1
SERVER_NAME = "mk3d-server"
SERVER_VERSION = __version__

MAX_LINE = 16 * 1024
MAX_DATAGRAM = 512
MAX_RELAY_DATAGRAM = 1472  # what the relay passes on: a game packet, up to a whole Ethernet frame of UDP payload

# game id -> online modes (practice is offline and never reaches the server)
GAMES: dict[str, tuple[str, ...]] = {
    "mkda": ("kombat",),
    "mkd": ("kombat", "chess", "puzzle"),
    "mka": ("kombat", "motor"),
}
REGIONS: tuple[str, ...] = ("na", "sa", "eu", "me", "af", "as", "oc")

NAME_RE = re.compile(r"^[A-Za-z0-9_-]{3,16}$")
CHARACTER_RE = re.compile(r"^[A-Za-z0-9_.-]{0,24}$")
TOKEN_RE = re.compile(r"^[0-9a-f]{16,128}$")
PASSWORD_MIN, PASSWORD_MAX = 6, 128
LOBBY_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
LOBBY_CODE_LEN = 5
LOBBY_NAME_MAX = 32
CHAT_MAX = 200
ROUNDS_ALLOWED = (1, 3, 5)
DELAY_MAX = 4


class ProtocolError(Exception):
    """A request that cannot be served; turned into an `error` message for the client."""

    def __init__(self, code: str, message: str = "") -> None:
        super().__init__(message or code)
        self.code = code
        self.message = message or code.replace("_", " ")


def encode(msg: dict[str, Any]) -> bytes:
    """One NDJSON line."""
    return (json.dumps(msg, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def encode_datagram(msg: dict[str, Any]) -> bytes:
    return json.dumps(msg, separators=(",", ":")).encode("utf-8")


def decode_line(line: bytes) -> dict[str, Any]:
    try:
        msg = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError("bad_json", "Message is not valid JSON.") from exc
    if not isinstance(msg, dict) or not isinstance(msg.get("type"), str):
        raise ProtocolError("bad_json", "Message must be a JSON object with a string 'type'.")
    return msg


def parse_version(text: str) -> tuple[int, ...]:
    parts = []
    for piece in str(text).split("."):
        digits = re.match(r"\d+", piece)
        parts.append(int(digits.group(0)) if digits else 0)
    return tuple(parts) or (0,)


def version_at_least(version: str, minimum: str) -> bool:
    a, b = parse_version(version), parse_version(minimum)
    width = max(len(a), len(b))
    return a + (0,) * (width - len(a)) >= b + (0,) * (width - len(b))


# where a launcher too old for the server is sent when no update file names a release
RELEASES_PAGE = "https://github.com/Jstackzz/MK-3D-rollback/releases/latest"


def download_link(url: Any) -> bool:
    """A link launchers may download from: https, or plain http to this PC (development)."""
    return isinstance(url, str) and len(url) <= 500 and (
        url.startswith("https://") or re.match(r"http://(127\.0\.0\.1|localhost)(:\d+)?/", url) is not None)


def parse_update(data: Any) -> dict[str, Any]:
    """The release launchers are offered (the server's update file); ValueError says what is wrong with it."""
    if not isinstance(data, dict):
        raise ValueError("not a JSON object")
    version, url, sha256, size = data.get("version"), data.get("url"), data.get("sha256"), data.get("size")
    notes, page = data.get("notes", ""), data.get("page", RELEASES_PAGE)
    if not isinstance(version, str) or not re.fullmatch(r"\d+(\.\d+)*", version):
        raise ValueError("version must look like 0.1.8")
    if not download_link(url):
        raise ValueError("url must be an https:// link to the release's zip")
    if not isinstance(sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise ValueError("sha256 must be the zip's SHA-256, 64 lowercase hex digits")
    if isinstance(size, bool) or not isinstance(size, int) or not 0 < size <= 1 << 30:
        raise ValueError("size must be the zip's size in bytes")
    if not isinstance(notes, str) or len(notes) > 2000:
        raise ValueError("notes must be text, 2000 characters at most")
    if not download_link(page):
        raise ValueError("page must be an https:// link")
    return {"version": version, "url": url, "sha256": sha256, "size": size, "notes": notes, "page": page}


def format_endpoint(addr: tuple[str, int] | None) -> str:
    if not addr:
        return ""
    return f"{addr[0]}:{addr[1]}"


def parse_endpoint(text: Any) -> tuple[str, int] | None:
    """'1.2.3.4:7001' -> ('1.2.3.4', 7001); None when malformed (IPv4 only in v1)."""
    if not isinstance(text, str) or text.count(":") != 1:
        return None
    host, _, port = text.partition(":")
    try:
        ip = ipaddress.IPv4Address(host)
        port_no = int(port)
    except ValueError:
        return None
    if not 0 < port_no < 65536:
        return None
    return str(ip), port_no


# --- field access -------------------------------------------------------------------------------


def get_str(msg: dict, key: str, *, default: str | None = None, max_len: int = 256) -> str:
    value = msg.get(key, default)
    if value is None or not isinstance(value, str):
        raise ProtocolError("bad_request", f"Field '{key}' must be a string.")
    if len(value) > max_len:
        raise ProtocolError("bad_request", f"Field '{key}' is too long.")
    return value


def get_bool(msg: dict, key: str, *, default: bool | None = None) -> bool:
    value = msg.get(key, default)
    if not isinstance(value, bool):
        raise ProtocolError("bad_request", f"Field '{key}' must be true or false.")
    return value


def get_int(msg: dict, key: str, *, lo: int, hi: int, default: int | None = None) -> int:
    value = msg.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError("bad_request", f"Field '{key}' must be an integer.")
    if not lo <= value <= hi:
        raise ProtocolError("bad_request", f"Field '{key}' must be between {lo} and {hi}.")
    return value


def get_game_mode(msg: dict) -> tuple[str, str]:
    game = msg.get("game")
    if game not in GAMES:
        raise ProtocolError("bad_game", f"Unknown game {game!r}.")
    mode = msg.get("mode")
    if mode not in GAMES[game]:
        raise ProtocolError("bad_mode", f"Mode {mode!r} is not an online mode of {game}.")
    return game, mode


def get_region(msg: dict, key: str = "region") -> str:
    region = msg.get(key)
    if region not in REGIONS:
        raise ProtocolError("bad_region", f"Unknown region {region!r}.")
    return region


def get_character(msg: dict, key: str = "character") -> str:
    value = msg.get(key, "")
    if value is None:
        value = ""
    if not isinstance(value, str) or not CHARACTER_RE.match(value):
        raise ProtocolError("bad_request", "Character ids are up to 24 letters, digits, '_', '.' or '-'.")
    return value
