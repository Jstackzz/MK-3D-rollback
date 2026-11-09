"""Ranked titles by Elo (0.1.21; the 16 badges' ladder since 2026-11-07): the ladder, and the title the server sends
with every rating, ranked or not (lobby members, a match's players)."""

from __future__ import annotations

import asyncio

from server import elo
from server.__main__ import RESET_RANKS_FLAG, reset_ranks_if_asked
from server.db import Database

from .conftest import hello_and_accept, queue_msg
from .test_lobby import create_msg
from .test_series import play

LADDER = [
    ("Beginner", None), ("Learner", 975), ("Fighter", 1475), ("Expert", 1975), ("Warrior", 2725),
    ("Gladiator", 3725), ("Master", 4725), ("Commander", 5725), ("Grandmaster", 7225), ("Warlord", 8725),
    ("Chosen One", 10725), ("Demi God", 12725), ("God", 15725), ("Elder God", 18725), ("Titan", 22225),
    ("Eldest God", 25725),
]


def test_titles_by_elo():
    assert [(r["title"], r["from"]) for r in elo.ladder()] == LADDER
    assert elo.title(elo.INITIAL) == "Fighter"  # where everyone starts
    for (name, floor), (_, upper) in zip(LADDER, LADDER[1:] + [(None, None)]):
        if floor is not None:
            assert elo.title(floor) == name
        if upper is not None:
            assert elo.title(upper - 1) == name
    assert elo.title(-40) == "Beginner" and elo.title(30000) == "Eldest God"


def set_rating(server, name, rating, game="mkd", mode="kombat"):
    account = server.db.account_by_name(name)["id"]
    server.db.conn.execute(
        "INSERT OR REPLACE INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
        "VALUES (?, ?, ?, 'ranked', ?, 3, 2, 0)", (account, game, mode, rating))


async def test_every_ranked_rating_comes_with_its_title(server, connect, udp_peers):
    conns = [await connect(n) for n in ("Fujin", "Raijin", "Shinnok")]
    assert conns[0].welcome["ranks"] == elo.ladder()  # the ladder, for the launcher to show
    for name, rating in (("Fujin", 2020), ("Raijin", 1960), ("Shinnok", 900)):
        set_rating(server, name, rating)

    prof = await conns[0].request({"type": "profile"})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 2020, "title": "Expert", "wins": 3, "losses": 2}
    board = await conns[2].request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert [(r["name"], r["elo"], r["title"]) for r in board["rows"]] == [
        ("Fujin", 2020, "Expert"), ("Raijin", 1960, "Fighter"), ("Shinnok", 900, "Beginner")]
    assert board["me"] == {"rank": 3, "elo": 900, "title": "Beginner", "wins": 3, "losses": 2}

    players = conns[:2]
    udps = [await udp_peers(), await udp_peers()]
    queued = [await c.request(queue_msg(ranked=True)) for c in players]
    assert [(q["elo"], q["title"]) for q in queued] == [(2020, "Expert"), (1960, "Fighter")]
    # the opponent stays hidden until both accept, then comes with their rating and its title
    founds = [await c.expect("match_found") for c in players]
    assert all(f["opponent"] is None for f in founds)
    for c, udp, found in zip(players, udps, founds):
        await hello_and_accept(server, c, udp, found)
    peers = [await c.expect("match_peer") for c in players]
    assert (peers[0]["opponent"]["elo"], peers[0]["opponent"]["title"]) == (1960, "Fighter")
    assert (peers[1]["opponent"]["elo"], peers[1]["opponent"]["title"]) == (2020, "Expert")
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for udp, peer in zip(udps, peers)])
    for c, peer, report in zip(players, peers, reports):
        c.send({"type": "ping_report", "match": peer["match"], **report})
    gos = [await c.expect("match_go") for c in players]

    # a set's state names the opponent's title; its end, each player's title before and after
    by_side = {go["side"]: c for c, go in zip(players, gos)}
    s1, s2 = await play(by_side[1], by_side[2], gos[0]["match"], 1)
    assert {s["opponent"]: s["opponent_title"] for s in (s1, s2)} == {"Raijin": "Fighter", "Fujin": "Expert"}
    left = await players[0].request({"type": "series_ready", "series": s1["series"], "ready": False})
    assert left["type"] == "series_left"
    over = await players[1].expect("series", state="over")
    assert over["winner"] == "Raijin"
    assert (over["elo"]["Fujin"]["title_before"], over["elo"]["Fujin"]["title_after"]) == ("Expert", "Expert")
    # up a rank, from beating an Expert who left the set
    assert over["elo"]["Raijin"] == {"before": 1960, "after": 1979, "delta": 19, "title_before": "Fighter",
                                     "title_after": "Expert"}


async def test_lobby_members_come_with_their_titles_in_its_game_and_mode(server, connect):
    host, guest = await connect("Kitana"), await connect("Jade")
    set_rating(server, "Kitana", 16000)  # a God at kombat
    set_rating(server, "Kitana", 900, mode="chess")  # a Beginner at chess kombat
    created = await host.request(create_msg())
    code = created["lobby"]["id"]
    assert [(m["name"], m["title"]) for m in created["lobby"]["members"]] == [("Kitana", "God")]
    # Jade never played ranked: the title everyone starts with
    joined = await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    assert [(m["name"], m["title"]) for m in joined["lobby"]["members"]] == [("Kitana", "God"), ("Jade", "Fighter")]
    await guest.request({"type": "lobby_leave"})
    await host.request({"type": "lobby_leave"})
    chess = await host.request(create_msg(mode="chess"))
    assert [(m["name"], m["title"]) for m in chess["lobby"]["members"]] == [("Kitana", "Beginner")]


EVERY_RECORD = [("mkd", "chess", "ranked", 1300), ("mkda", "kombat", "casual", 1500), ("mkda", "kombat", "ranked", 1700)]


def database_with_records(tmp_path):
    path = str(tmp_path / "server.sqlite")
    db = Database(path)
    account = db.create_account("Fujin", "x")
    for game, mode, kind, rating in EVERY_RECORD:
        db.conn.execute("INSERT INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
                        "VALUES (?, ?, ?, ?, ?, 5, 4, 0)", (account, game, mode, kind, rating))
    db.close()
    return path, account


def records(path):
    db = Database(path)
    rows = [tuple(r) for r in db.conn.execute("SELECT game, mode, kind, elo FROM stats ORDER BY game, mode, kind")]
    db.close()
    return rows


def test_a_rank_reset_happens_once_when_asked_and_keeps_a_copy(tmp_path):
    path, account = database_with_records(tmp_path)
    flag = tmp_path / RESET_RANKS_FLAG

    def copies():
        return [c for c in tmp_path.glob("server.sqlite.before-rank-reset-*") if not c.name.endswith(("-wal", "-shm"))]

    reset_ranks_if_asked(path)  # not asked: nothing happens
    assert records(path) == EVERY_RECORD and not copies()

    flag.touch()
    reset_ranks_if_asked(path)
    assert records(path) == [("mkda", "kombat", "casual", 1500)]  # the Kasual record stays
    assert not flag.exists()
    assert [records(str(c)) for c in copies()] == [EVERY_RECORD]  # the database as it was
    db = Database(path)
    assert db.rating(account, "mkda", "kombat") == {"elo": elo.INITIAL, "wins": 0, "losses": 0}  # a new player's
    assert db.account_by_name("Fujin") is not None
    db.close()

    reset_ranks_if_asked(path)  # the next start does not do it again
    assert len(copies()) == 1


def test_a_rank_reset_that_cannot_copy_the_database_clears_nothing(tmp_path, monkeypatch):
    path, _ = database_with_records(tmp_path)
    flag = tmp_path / RESET_RANKS_FLAG
    flag.touch()

    def no_room(self, copy):
        raise OSError("no space left on the device")

    monkeypatch.setattr(Database, "backup", no_room)
    reset_ranks_if_asked(path)  # the server still starts
    assert records(path) == EVERY_RECORD and flag.exists()
