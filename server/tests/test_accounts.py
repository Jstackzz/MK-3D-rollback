"""hello/version check, register/login/auth/logout, password storage, TLS pinning, rate limit."""

from __future__ import annotations

import pytest

from server import auth
from server import protocol as P
from server.client import Connection, FingerprintMismatch, ServerError

from .conftest import PASSWORD


async def test_welcome_and_version_check(server, connect):
    conn = await connect()
    assert conn.welcome["type"] == "welcome"
    assert conn.welcome["proto"] == P.PROTO
    assert conn.welcome["udp_port"] == server.udp_port
    assert conn.welcome["games"]["mkd"] == ["kombat", "chess", "puzzle"]
    assert conn.welcome["games"]["mka"] == ["kombat", "motor"]
    assert conn.welcome["games"]["mkda"] == ["kombat"]

    bad_proto = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    await bad_proto.connect(hello=False)
    with pytest.raises(ServerError) as exc:
        await bad_proto.request({"type": "hello", "proto": 99, "version": "0.1.0"})
    assert exc.value.code == "proto_mismatch"
    await bad_proto.close()

    old = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    with pytest.raises(ServerError) as exc:
        await old.connect(version="0.0.9")
    assert exc.value.code == "client_outdated"
    await old.close()

    early = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    await early.connect(hello=False)
    with pytest.raises(ServerError) as exc:
        await early.request({"type": "login", "name": "x", "password": "y"})
    assert exc.value.code == "need_hello"
    await early.close()


async def test_pinned_fingerprint(server):
    wrong = Connection("127.0.0.1", server.tcp_port, fingerprint="00" * 32)
    with pytest.raises(FingerprintMismatch):
        await wrong.connect()
    right = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint.lower().replace(":", ""))
    await right.connect()
    assert right.peer_fingerprint == server.fingerprint.lower().replace(":", "")
    await right.close()


async def test_register_login_auth_logout(server, connect):
    alice = await connect("Alice")
    acc = alice.account
    assert acc["type"] == "account" and acc["name"] == "Alice"
    assert len(acc["token"]) == 64 and len(acc["udp_token"]) == 32
    assert acc["profile"]["stats"] == {}

    # names are unique without regard to case
    other = await connect()
    with pytest.raises(ServerError) as exc:
        await other.register("alice", PASSWORD)
    assert exc.value.code == "name_taken"
    for name, password, code in (("a!", PASSWORD, "bad_name"), ("Bob", "short", "bad_password")):
        with pytest.raises(ServerError) as exc:
            await other.register(name, password)
        assert exc.value.code == code

    with pytest.raises(ServerError) as exc:
        await other.login("Alice", "wrong-password")
    assert exc.value.code == "bad_login"
    with pytest.raises(ServerError) as exc:
        await other.login("Nobody", PASSWORD)
    assert exc.value.code == "bad_login"

    # a second login takes the account over and closes the first connection
    await other.login("alice", PASSWORD)
    assert other.account["name"] == "Alice"
    kicked = await alice.expect("error", code="logged_in_elsewhere")
    assert kicked["code"] == "logged_in_elsewhere"

    # the remembered token logs in without the password, until logout revokes it
    third = await connect()
    resumed = await third.request({"type": "auth", "token": acc["token"]})
    assert resumed["name"] == "Alice" and resumed["token"] == acc["token"]
    await third.request({"type": "logout"})
    fourth = await connect()
    with pytest.raises(ServerError) as exc:
        await fourth.request({"type": "auth", "token": acc["token"]})
    assert exc.value.code == "bad_token"


async def test_passwords_are_salted_scrypt_hashes(server, connect):
    await connect("Carol")
    await connect("Dave")
    rows = {r["name"]: r["pw_hash"] for r in server.db.conn.execute("SELECT name, pw_hash FROM accounts")}
    for stored in rows.values():
        scheme, n, r, p, salt, digest = stored.split("$")
        assert (scheme, n, r, p) == ("scrypt", "16384", "8", "1")
        assert len(bytes.fromhex(salt)) == 16 and len(bytes.fromhex(digest)) == 32
        assert PASSWORD not in stored
        assert auth.verify_password(PASSWORD, stored)
        assert not auth.verify_password(PASSWORD + "x", stored)
    assert rows["Carol"] != rows["Dave"]  # same password, different salt
    tokens = [r[0] for r in server.db.conn.execute("SELECT token_hash FROM tokens")]
    assert len(tokens) == 2 and all(len(t) == 64 for t in tokens)


async def test_profile_and_requires_login(server, connect):
    anon = await connect()
    with pytest.raises(ServerError) as exc:
        await anon.request({"type": "queue_join", "game": "mkd", "mode": "kombat", "region": "eu"})
    assert exc.value.code == "not_logged_in"
    await connect("Erin")
    prof = await anon.request({"type": "profile", "name": "erin"})
    assert prof["name"] == "Erin" and prof["stats"] == {}
    with pytest.raises(ServerError) as exc:
        await anon.request({"type": "profile", "name": "ghost"})
    assert exc.value.code == "no_such_account"


async def test_failed_login_rate_limit(server, connect):
    server.cfg.login_fail_limit = 3
    await connect("Frank")
    conn = await connect()
    for _ in range(3):
        with pytest.raises(ServerError) as exc:
            await conn.login("Frank", "nope-nope")
        assert exc.value.code == "bad_login"
    with pytest.raises(ServerError) as exc:
        await conn.login("Frank", PASSWORD)
    assert exc.value.code == "rate_limited"


async def test_online_count_and_notice(server, connect):
    server.cfg.admins = ("Root",)
    admin = await connect("Root")
    user = await connect("Gina")
    online = await user.request({"type": "online"})
    assert online["count"] == 2 and online["connected"] >= 2
    with pytest.raises(ServerError) as exc:
        await user.request({"type": "admin_notice", "text": "hi"})
    assert exc.value.code == "not_admin"
    admin.send({"type": "admin_notice", "text": "Restart soon", "level": "warn"})
    notice = await user.expect("notice")
    assert notice["text"] == "Restart soon" and notice["level"] == "warn"
    pushed = await user.expect("online", timeout=3, pred=lambda m: "id" not in m)
    assert pushed["count"] >= 1


async def test_bad_input_is_answered(server, connect):
    conn = await connect()
    conn.send({"type": "no_such_thing", "id": 5})
    err = await conn.expect("error", id=5)
    assert err["code"] == "unknown_type"
    conn.writer.write(b"{not json}\n")
    err = await conn.expect("error", code="bad_json")
    assert err["code"] == "bad_json"


async def test_a_crash_report_is_logged_with_the_player(server, connect, caplog):
    """A launcher whose game crashed says where; the server logs it with the player (nothing kept), refuses what is
    not plain text, and stops listening after a session's share."""
    c = await connect("Raiden")
    with caplog.at_level("INFO", logger="mk3d.server"):
        ack = await c.request({"type": "crash_report", "game": "mka", "code": "C0000005",
                               "where": "MKA.exe+1A2B3C on a game thread", "during": "Playing"})
    assert ack["type"] == "crash_ack"
    assert any("CRASH Raiden mka" in r.getMessage() and "C0000005 MKA.exe+1A2B3C" in r.getMessage()
               for r in caplog.records)
    with pytest.raises(ServerError) as exc:
        await c.request({"type": "crash_report", "game": "mka", "code": "C0000005", "where": "bad\x07line"})
    assert exc.value.code == "bad_request"
    with pytest.raises(ServerError) as exc:
        await c.request({"type": "crash_report", "game": "nope", "code": "C0000005"})
    assert exc.value.code == "bad_request"
