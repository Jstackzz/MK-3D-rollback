"""Ranked titles by Elo (0.1.21; the 16 badges' ladder since 2026-11-07): the ladder, and the title the server sends
with every rating, ranked or not (lobby members, a match's players)."""

from __future__ import annotations

import asyncio

from server import elo
from server import protocol as P
from server.__main__ import RESET_RANKS_FLAG, RESTORE_RECORDS_FLAG, reset_ranks_if_asked, restore_ranked_records_if_asked
from server.db import Database

from .conftest import hello_and_accept, queue_msg
from .test_lobby import create_msg
from .test_series import play

LADDER = [
    ("Beginner", None), ("Learner", 575), ("Fighter", 675), ("Expert", 775), ("Warrior", 875), ("Gladiator", 975),
    ("Master", 1075), ("Commander", 1175), ("Grandmaster", 1275), ("Warlord", 1375), ("Chosen One", 1475),
    ("Demi God", 1575), ("God", 1675), ("Elder God", 1795), ("Titan", 1915), ("Eldest God", 2035),
]


def test_titles_by_elo():
    assert [(r["title"], r["from"]) for r in elo.ladder()] == LADDER
    assert elo.title(elo.INITIAL) == "Beginner"  # where a new player starts
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
    for name, rating in (("Fujin", 1840), ("Raijin", 1780), ("Shinnok", 500)):
        set_rating(server, name, rating)

    prof = await conns[0].request({"type": "profile"})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1840, "title": "Elder God", "wins": 3, "losses": 2}
    board = await conns[2].request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert [(r["name"], r["elo"], r["title"]) for r in board["rows"]] == [
        ("Fujin", 1840, "Elder God"), ("Raijin", 1780, "God"), ("Shinnok", 500, "Beginner")]
    assert board["me"] == {"rank": 3, "elo": 500, "title": "Beginner", "wins": 3, "losses": 2}

    players = conns[:2]
    udps = [await udp_peers(), await udp_peers()]
    queued = [await c.request(queue_msg(ranked=True)) for c in players]
    assert [(q["elo"], q["title"]) for q in queued] == [(1840, "Elder God"), (1780, "God")]
    # the opponent stays hidden until both accept, then comes with their rating and its title
    founds = [await c.expect("match_found") for c in players]
    assert all(f["opponent"] is None for f in founds)
    for c, udp, found in zip(players, udps, founds):
        await hello_and_accept(server, c, udp, found)
    peers = [await c.expect("match_peer") for c in players]
    assert (peers[0]["opponent"]["elo"], peers[0]["opponent"]["title"]) == (1780, "God")
    assert (peers[1]["opponent"]["elo"], peers[1]["opponent"]["title"]) == (1840, "Elder God")
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for udp, peer in zip(udps, peers)])
    for c, peer, report in zip(players, peers, reports):
        c.send({"type": "ping_report", "match": peer["match"], **report})
    gos = [await c.expect("match_go") for c in players]

    # a set's state names the opponent's title; its end, each player's title before and after
    by_side = {go["side"]: c for c, go in zip(players, gos)}
    s1, s2 = await play(by_side[1], by_side[2], gos[0]["match"], 1)
    assert {s["opponent"]: s["opponent_title"] for s in (s1, s2)} == {"Raijin": "God", "Fujin": "Elder God"}
    left = await players[0].request({"type": "series_ready", "series": s1["series"], "ready": False})
    assert left["type"] == "series_left"
    over = await players[1].expect("series", state="over")
    assert over["winner"] == "Raijin"
    assert (over["elo"]["Fujin"]["title_before"], over["elo"]["Fujin"]["title_after"]) == ("Elder God", "Elder God")
    # up a rank, God to Elder God (a 120 Elo step), from beating an Elder God who left the set
    assert over["elo"]["Raijin"] == {"before": 1780, "after": 1799, "delta": 19, "title_before": "God",
                                     "title_after": "Elder God"}


async def test_lobby_members_come_with_their_titles_in_its_game_and_mode(server, connect):
    host, guest = await connect("Kitana"), await connect("Jade")
    set_rating(server, "Kitana", 1700)  # a God at kombat
    set_rating(server, "Kitana", 500, mode="chess")  # a Beginner at chess kombat
    created = await host.request(create_msg())
    code = created["lobby"]["id"]
    assert [(m["name"], m["title"]) for m in created["lobby"]["members"]] == [("Kitana", "God")]
    # Jade never played ranked: the title everyone starts with
    joined = await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    assert [(m["name"], m["title"]) for m in joined["lobby"]["members"]] == [("Kitana", "God"), ("Jade", "Beginner")]
    await guest.request({"type": "lobby_leave"})
    await host.request({"type": "lobby_leave"})
    chess = await host.request(create_msg(mode="chess"))
    assert [(m["name"], m["title"]) for m in chess["lobby"]["members"]] == [("Kitana", "Beginner")]


async def test_a_new_player_starts_a_beginner_and_a_record_from_the_old_start_stays(server, connect):
    c = await connect("Goro")
    assert elo.INITIAL == 475
    start = {"elo": 475, "title": "Beginner", "wins": 0, "losses": 0}
    prof = await c.request({"type": "profile"})
    assert all(prof["stats"][game][mode]["ranked"] == start for game, modes in P.GAMES.items() for mode in modes)
    set_rating(server, "Goro", 1516)  # played ranked while everyone started at 1500
    prof = await c.request({"type": "profile"})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1516, "title": "Chosen One", "wins": 3, "losses": 2}
    assert prof["stats"]["mkd"]["chess"]["ranked"] == start  # each mode has its own


EVERY_RECORD = [("mka", "kombat", "ranked", 1640), ("mkd", "chess", "ranked", 1300), ("mkda", "kombat", "casual", 1500),
                ("mkda", "kombat", "ranked", 1700)]


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


def full_records(path):
    db = Database(path)
    rows = [tuple(r) for r in db.conn.execute("SELECT game, mode, kind, elo, wins, losses FROM stats ORDER BY game, mode, kind")]
    db.close()
    return rows


def test_restoring_brings_back_elo_and_records_in_every_game(tmp_path):
    path, account = database_with_records(tmp_path)
    (tmp_path / RESET_RANKS_FLAG).touch()
    reset_ranks_if_asked(path)
    db = Database(path)
    for game, mode, rating, wins, losses in (("mkda", "kombat", 491, 2, 1),  # played since the reset
                                             ("mkd", "kombat", 507, 3, 0)):  # a record that did not exist before it
        db.conn.execute("INSERT INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
                        "VALUES (?, ?, ?, 'ranked', ?, ?, ?, 0)", (account, game, mode, rating, wins, losses))
    db.close()

    flag = tmp_path / RESTORE_RECORDS_FLAG
    restore_ranked_records_if_asked(path)  # not asked: nothing happens
    assert ("mkda", "kombat", "ranked", 491, 2, 1) in full_records(path)

    flag.touch()
    restore_ranked_records_if_asked(path)
    assert not flag.exists()
    expected = [("mka", "kombat", "ranked", 1640, 5, 4),  # nothing played since: as it was
                ("mkd", "chess", "ranked", 1300, 5, 4),
                ("mkd", "kombat", "ranked", 507, 3, 0),  # not in the copy: stays
                ("mkda", "kombat", "casual", 1500, 5, 4),
                ("mkda", "kombat", "ranked", 1700, 7, 5)]  # played since: the Elo it had, the records add up
    assert full_records(path) == expected

    restore_ranked_records_if_asked(path)  # the next start does not add them again
    assert full_records(path) == expected
    assert [c for c in tmp_path.glob("server.sqlite.before-records-restore-*") if not c.name.endswith(("-wal", "-shm"))]


def test_a_rank_reset_that_cannot_copy_the_database_clears_nothing(tmp_path, monkeypatch):
    path, _ = database_with_records(tmp_path)
    flag = tmp_path / RESET_RANKS_FLAG
    flag.touch()

    def no_room(self, copy):
        raise OSError("no space left on the device")

    monkeypatch.setattr(Database, "backup", no_room)
    reset_ranks_if_asked(path)  # the server still starts
    assert records(path) == EVERY_RECORD and flag.exists()
