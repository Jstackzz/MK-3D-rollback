"""Result reporting: agreement counts wins (Elo moves per ranked set, test_series.py), disagreement changes nothing
and is logged."""

from __future__ import annotations

import logging

from server import elo

from .conftest import queue_msg, run_to_go


async def setup_match(server, connect, udp_peers, names, *, ranked, game="mkd", mode="kombat"):
    conns = [await connect(n) for n in names]
    udps = [await udp_peers(), await udp_peers()]
    for conn in conns:
        await conn.request(queue_msg(game=game, mode=mode, ranked=ranked))
    gos = await run_to_go(server, list(zip(conns, udps)))
    by_side = {go["side"]: (conn, go) for conn, go in zip(conns, gos)}
    return by_side, gos[0]["match"]


def report(conn, match, winner, r1, r2):
    return conn.request({"type": "result", "match": match, "winner": winner, "p1_rounds": r1, "p2_rounds": r2})


async def test_ranked_agreement_counts_the_match(server, connect, udp_peers):
    sides, match = await setup_match(server, connect, udp_peers, ["Shujinko", "Onaga"], ranked=True)
    (c1, go1), (c2, _) = sides[1], sides[2]
    ack = await report(c1, match, 1, 2, 1)
    assert ack["type"] == "result_ack"
    await report(c2, match, 1, 2, 1)
    r1, r2 = await c1.expect("match_result"), await c2.expect("match_result")
    assert r1 == {**r2, "type": "match_result"}
    assert r1["status"] == "agreed" and r1["winner"] == 1 and (r1["p1_rounds"], r1["p2_rounds"]) == (2, 1)
    assert "elo" not in r1  # the set it opened decides the rating
    winner, loser = go1["p1_name"], go1["p2_name"]

    prof = await c1.request({"type": "profile", "name": winner})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1500, "wins": 1, "losses": 0}
    prof = await c1.request({"type": "profile", "name": loser})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1500, "wins": 0, "losses": 1}

    board = await c2.request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert board["total"] == 2
    assert [(r["rank"], r["name"], r["elo"]) for r in board["rows"]] == [(1, winner, 1500), (2, loser, 1500)]
    assert board["me"]["rank"] in (1, 2)
    empty = await c2.request({"type": "leaderboard", "game": "mkd", "mode": "chess"})
    assert empty["rows"] == [] and empty["total"] == 0

    row = server.db.match_row(match)
    assert row["status"] == "agreed" and row["p1_elo_after"] is None
    assert elo.delta(1500, 1500, 1.0) == 16


async def test_ranked_disagreement_changes_nothing(server, connect, udp_peers, caplog):
    sides, match = await setup_match(server, connect, udp_peers, ["Darrius", "Dairou"], ranked=True)
    (c1, go1), (c2, _) = sides[1], sides[2]
    with caplog.at_level(logging.WARNING, logger="mk3d.server"):
        await report(c1, match, 1, 2, 0)
        await report(c2, match, 2, 1, 2)  # each claims the win
        r1 = await c1.expect("match_result")
    await c2.expect("match_result")
    assert r1["status"] == "disputed" and r1["winner"] is None and "elo" not in r1
    assert any("DISPUTE" in rec.getMessage() and match in rec.getMessage() for rec in caplog.records)
    for name in (go1["p1_name"], go1["p2_name"]):
        prof = await c1.request({"type": "profile", "name": name})
        assert prof["stats"] == {}
    assert server.db.match_row(match)["status"] == "disputed"
    stored = server.db.reports(match)
    assert [(r["side"], r["winner"]) for r in stored] == [(1, 1), (2, 2)]
    board = await c1.request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert board["rows"] == []


async def test_casual_agreement_counts_wins_only(server, connect, udp_peers):
    sides, match = await setup_match(server, connect, udp_peers, ["Kobra", "Havik"], ranked=False)
    (c1, go1), (c2, _) = sides[1], sides[2]
    await report(c1, match, 2, 1, 2)
    await report(c2, match, 2, 1, 2)
    r1 = await c1.expect("match_result")
    assert r1["status"] == "agreed" and r1["winner"] == 2 and "elo" not in r1
    prof = await c1.request({"type": "profile", "name": go1["p2_name"]})
    assert prof["stats"] == {"mkd": {"kombat": {"casual": {"wins": 1, "losses": 0}}}}
    prof = await c1.request({"type": "profile", "name": go1["p1_name"]})
    assert prof["stats"] == {"mkd": {"kombat": {"casual": {"wins": 0, "losses": 1}}}}


async def test_desync_report_is_no_contest(server, connect, udp_peers):
    sides, match = await setup_match(server, connect, udp_peers, ["Sindel", "Ashrah"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    await c1.request({"type": "result", "match": match, "outcome": "desync", "frame": 4242})
    await report(c2, match, 1, 2, 0)
    r1 = await c1.expect("match_result")
    assert r1["status"] == "no_contest"
    assert server.db.rating(server.db.account_by_name("Sindel")["id"], "mkd", "kombat")["elo"] == 1500


async def test_single_report_times_out_unconfirmed(server, connect, udp_peers):
    server.cfg.result_timeout = 0.3
    sides, match = await setup_match(server, connect, udp_peers, ["Nightwolf", "Kai"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    await report(c1, match, 1, 2, 0)
    # the reporter is free to queue again while the other report is outstanding
    queued = await c1.request(queue_msg(ranked=True))
    assert queued["type"] == "queued"
    r1 = await c1.expect("match_result", timeout=3)
    assert r1["status"] == "unconfirmed"
    assert server.db.match_row(match)["status"] == "unconfirmed"
    # a late duplicate report is acknowledged, not an error
    late = await report(c2, match, 1, 2, 0)
    assert late["type"] == "result_ack"


async def test_elo_math():
    assert elo.update(1500, 1500, 1.0) == (1516, 1484)
    assert elo.update(1600, 1400, 1.0) == (1608, 1392)
    assert elo.update(1400, 1600, 1.0) == (1424, 1576)
    a, b = elo.update(1523, 1611, 0.0)
    assert a + b == 1523 + 1611
