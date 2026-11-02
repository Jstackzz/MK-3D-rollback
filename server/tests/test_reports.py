"""F3 reports: a launcher sends what its player saw drawn wrong (the game's capture, its log) in parts; the server
writes each file once its parts are in, under its reports folder, and logs the report with the player."""

from __future__ import annotations

import base64
import os
import struct
import zlib

import pytest

from server.client import ServerError

RID = "0123456789abcdef"


def qcompress(data: bytes) -> bytes:
    """As Qt's qCompress: the length big-endian, then zlib."""
    return struct.pack(">I", len(data)) + zlib.compress(data)


def part(name, data: bytes, index, count, *, rid=RID, game="mka", compressed=False):
    msg = {"type": "report_file", "report": rid, "game": game, "name": name, "part": index, "parts": count,
           "data": base64.b64encode(data).decode()}
    if compressed:
        msg["zlib"] = True
    return msg


async def test_a_report_is_written_and_logged(server, connect, tmp_path, caplog):
    server.cfg.reports_dir = str(tmp_path / "reports")
    c = await connect("Raiden")
    picture = os.urandom(20000)
    c.send(part("final.jpg", picture[:12000], 0, 2))
    c.send(part("final.jpg", picture[12000:], 1, 2))
    text = b"0000 draw prim 5 count 12\n" * 2000
    c.send(part("frame.txt", qcompress(text), 0, 1, compressed=True))
    with caplog.at_level("INFO", logger="mk3d.server"):
        saved = await c.request({"type": "report_done", "report": RID})
    assert saved["type"] == "report_saved" and saved["files"] == 2
    [day] = os.listdir(tmp_path / "reports")
    [folder] = os.listdir(tmp_path / "reports" / day)
    assert folder.endswith("-Raiden-mka-" + RID)
    where = tmp_path / "reports" / day / folder
    assert (where / "final.jpg").read_bytes() == picture
    assert (where / "frame.txt").read_bytes() == text
    assert any("REPORT Raiden mka" in r.getMessage() and "2 files" in r.getMessage() for r in caplog.records)


async def test_report_parts_come_in_order_within_limits(server, connect, tmp_path, monkeypatch):
    server.cfg.reports_dir = str(tmp_path / "reports")
    c = await connect("Kitana")
    for bad in (part("final.exe", b"x", 0, 1), part("../up.jpg", b"x", 0, 1), part("a.jpg", b"x", 1, 1),
                part("a.jpg", b"x", 0, 1, game="sf2"), {**part("a.jpg", b"x", 0, 1), "data": "not base64!"}):
        with pytest.raises(ServerError) as exc:
            await c.request(bad)
        assert exc.value.code == "bad_request"
    with pytest.raises(ServerError) as exc:  # nothing came: nothing to finish
        await c.request({"type": "report_done", "report": "fedcba9876543210"})
    assert exc.value.code == "bad_request"
    # a part that skips one
    c.send(part("next_00.jpg", b"abc", 0, 3, rid="1111111111111111"))
    with pytest.raises(ServerError) as exc:
        await c.request(part("next_00.jpg", b"abc", 2, 3, rid="1111111111111111"))
    assert exc.value.code == "bad_request"
    # too large: the report's files together
    monkeypatch.setattr("server.app.REPORT_MAX_BYTES", 1000)
    with pytest.raises(ServerError) as exc:
        await c.request(part("big.png", b"x" * 1001, 0, 1, rid="2222222222222222"))
    assert exc.value.code == "too_large"
    # a compressed file that inflates past the limit
    with pytest.raises(ServerError) as exc:
        await c.request(part("log.txt", qcompress(b"y" * 5000), 0, 1, rid="2222222222222222", compressed=True))
    assert exc.value.code == "too_large"
    # three reports a connection
    with pytest.raises(ServerError) as exc:
        await c.request(part("a.jpg", b"x", 0, 1, rid="3333333333333333"))
    assert exc.value.code == "too_many"


async def test_a_server_without_a_reports_folder_takes_none(server, connect):
    server.cfg.reports_dir = None
    c = await connect("Jax")
    with pytest.raises(ServerError) as exc:
        await c.request(part("final.jpg", b"x", 0, 1))
    assert exc.value.code == "unavailable"
