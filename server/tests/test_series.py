"""Series after a queue match: casual rematches while both want one, with the same fighters or both picking again;
ranked sets to two match wins, Elo moving once per set, and a player who leaves a set with a match counted loses it."""

from __future__ import annotations

import pytest

from server.client import ServerError

from .conftest import queue_msg, run_to_go


async def first_match(server, connect, udp_peers, names, *, ranked, character="Scorpion"):
    conns = [await connect(n) for n in names]
    udps = [await udp_peers(), await udp_peers()]
    for conn in conns:
        await conn.request(queue_msg(ranked=ranked, character=character))
    gos = await run_to_go(server, list(zip(conns, udps)))
    sides = {go["side"]: (conn, udp) for conn, udp, go in zip(conns, udps, gos)}
    return sides, {go["side"]: go for go in gos}


async def play(c1, c2, match, winner, fighters=(None, None)):
    """Both players report the same result (each with the fighters its game saw, by player); returns the series
    message each gets after it."""
    p1_rounds, p2_rounds = (2, 0) if winner == 1 else (0, 2)
    for c, seen in zip((c1, c2), fighters):
        report = {"type": "result", "match": match, "winner": winner, "p1_rounds": p1_rounds, "p2_rounds": p2_rounds}
        if seen is not None:
            report.update(p1_char=seen[0], p2_char=seen[1])
        await c.request(report)
    for c in (c1, c2):
        assert (await c.expect("match_result", match=match))["status"] == "agreed"
    return await c1.expect("series"), await c2.expect("series")


async def next_match(server, sides, series_id, choices=(None, None)):
    """Both choose the next match (a rematch unless a choice is given, by side); returns both match_go by side."""
    (c1, u1), (c2, u2) = sides[1], sides[2]
    for c, choice in zip((c1, c2), choices):
        msg = {"type": "series_ready", "series": series_id, "ready": True}
        if choice is not None:
            msg["choice"] = choice
        assert (await c.request(msg))["type"] == "series_ack"
    gos = await run_to_go(server, [(c1, u1), (c2, u2)])
    for c in (c1, c2):
        c.take_all("series")
    return {go["side"]: go for go in gos}


async def ranked_stats(conn, name):
    prof = await conn.request({"type": "profile", "name": name})
    return prof["stats"]["mkd"]["kombat"]["ranked"]


async def test_ranked_set_ends_at_two_wins_with_one_elo_change(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Shujinko", "Onaga"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, s2 = await play(c1, c2, gos[1]["match"], 1)
    assert s1["state"] == "between" and s1["ranked"] and s1["need"] == 2 and s1["opponent"] == p2
    assert (s1["you"], s1["them"], s2["you"], s2["them"]) == (1, 0, 0, 1)
    # the match counts at once, the rating waits for the set
    assert await ranked_stats(c1, p1) == {"elo": 1500, "wins": 1, "losses": 0}

    gos2 = await next_match(server, sides, s1["series"])
    assert gos2[1]["match"] != gos[1]["match"] and gos2[1]["p1_name"] == p1  # same sides
    over1, over2 = await play(c1, c2, gos2[1]["match"], 1)
    assert over1["state"] == "over" and over1["outcome"] == "won" and over1["winner"] == p1
    assert (over1["you"], over1["them"]) == (2, 0)
    assert over1["elo"][p1] == {"before": 1500, "after": 1516, "delta": 16}
    assert over2["elo"][p2] == {"before": 1500, "after": 1484, "delta": -16}
    assert await ranked_stats(c1, p1) == {"elo": 1516, "wins": 2, "losses": 0}
    assert await ranked_stats(c1, p2) == {"elo": 1484, "wins": 0, "losses": 2}
    row = server.db.set_row(over1["series"])
    assert row["outcome"] == "won" and (row["p1_wins"], row["p2_wins"]) == (2, 0)
    assert (row["p1_elo_after"], row["p2_elo_after"]) == (1516, 1484)
    assert not server.series and not server.series_of


async def test_one_match_won_and_gone_loses_the_set(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kenshi", "Mavado"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 1)  # P1 wins the first match...
    left = await c1.request({"type": "series_ready", "series": s1["series"], "ready": False})  # ...and leaves
    assert left["type"] == "series_left"
    over = await c2.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p1 and over["winner"] == p2
    assert over["elo"][p1]["delta"] == -16 and over["elo"][p2]["delta"] == 16
    assert await ranked_stats(c2, p1) == {"elo": 1484, "wins": 1, "losses": 0}
    assert await ranked_stats(c2, p2) == {"elo": 1516, "wins": 0, "losses": 1}


async def test_searching_again_in_a_set_loses_it(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Tanya", "Hotaru"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    await play(c1, c2, gos[1]["match"], 1)
    assert (await c1.request(queue_msg(ranked=True)))["type"] == "queued"
    over = await c2.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p1


async def test_a_set_waits_for_the_next_match_then_the_absent_one_loses(server, connect, udp_peers):
    server.cfg.set_next_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["Ashrah", "Havik"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 2)
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": True})
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1


async def test_leaving_before_anything_counted_changes_no_rating(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kabal", "Mileena"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    match = gos[1]["match"]
    await c1.request({"type": "result", "match": match, "outcome": "desync", "frame": 30})
    await c2.request({"type": "result", "match": match, "outcome": "desync", "frame": 30})
    s1 = await c1.expect("series")
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (0, 0)
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": False})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "void" and over["winner"] is None and "elo" not in over
    assert server.db.rating(server.db.account_by_name("Kabal")["id"], "mkd", "kombat")["elo"] == 1500


async def test_casual_rematch_while_both_want_one(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kobra", "Kira"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, s2 = await play(c1, c2, gos[1]["match"], 2)
    assert s1["state"] == "between" and not s1["ranked"] and s1["need"] == 0
    assert (s1["you"], s1["them"], s2["you"], s2["them"]) == (0, 1, 1, 0)

    # the first to choose a rematch is shown waiting to the other
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": True})
    waiting = await c1.expect("series", state="between", them_ready=True)
    assert waiting["you_ready"] is False
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": True})
    gos2 = await run_to_go(server, [sides[1], sides[2]])
    gos2 = {go["side"]: go for go in gos2}
    assert gos2[1]["p1_name"] == p1 and gos2[2]["p2_name"] == p2
    for c in (c1, c2):
        c.take_all("series")

    s1, _ = await play(c1, c2, gos2[1]["match"], 1)
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (1, 1) and s1["played"] == 2
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": False})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "left" and over["left"] == p2 and "elo" not in over
    prof = await c1.request({"type": "profile", "name": p1})
    assert prof["stats"] == {"mkd": {"kombat": {"casual": {"wins": 1, "losses": 1}}}}


async def test_casual_rematch_keeps_the_fighters_and_select_picks_again(server, connect, udp_peers):
    # both pick on the select screen ("" queued); the games report what was picked
    sides, gos = await first_match(server, connect, udp_peers, ["Raiden", "Jade"], ranked=False, character="")
    (c1, _), (c2, _) = sides[1], sides[2]
    assert (gos[1]["p1"], gos[1]["p2"]) == ("-", "-")
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("12", "7"), ("12", "7")))
    assert s1["same_fighters"] is True and s1["you_choice"] == "" and s1["them_choice"] == ""

    # a rematch: the same fighters, picked by the autopilot
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("12", "7") and (gos2[2]["p1"], gos2[2]["p2"]) == ("12", "7")
    s1, _ = await play(c1, c2, gos2[1]["match"], 2, fighters=(("12", "7"), ("12", "7")))

    # one wants character select: the other sees it, and both pick again
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": True, "choice": "select"})
    waiting = await c1.expect("series", state="between", them_ready=True)
    assert waiting["them_choice"] == "select" and waiting["you_choice"] == ""
    gos3 = await next_match(server, sides, s1["series"], choices=("rematch", None))
    assert (gos3[1]["p1"], gos3[1]["p2"]) == ("-", "-")


async def test_casual_rematch_without_agreed_fighters_uses_the_queued_ones(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kai", "Sareena"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    # the two games saw different fighters (or an old launcher sent none): nothing to repeat
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("3", "4"), ("3", "5")))
    assert s1["same_fighters"] is False
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("Scorpion", "Scorpion")


async def test_ranked_set_keeps_the_queued_characters(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Dairou", "Darrius"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("3", "4"), ("3", "4")))
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("Scorpion", "Scorpion")


async def test_bad_fighters_or_choice(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Nitara", "Noob"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    # a fighter id the server does not take never costs the result
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("bad id!", "4"), ("3", "4")))
    assert s1["same_fighters"] is False
    with pytest.raises(ServerError) as exc:
        await c1.request({"type": "series_ready", "series": s1["series"], "ready": True, "choice": "random"})
    assert exc.value.code == "bad_request"


async def test_casual_rematch_offer_runs_out(server, connect, udp_peers):
    server.cfg.rematch_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["BoRaiCho", "Shinnok"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    await play(c1, c2, gos[1]["match"], 1)
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "timeout" and over["winner"] is None
    # free to search again
    assert (await c1.request(queue_msg()))["type"] == "queued"


async def test_closing_the_game_mid_match_offers_the_kasual_rematch_again(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["BoRai", "Kotal"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    match = gos[1]["match"]
    # P1's game closes during the match: the match ends for both at once and counts for nobody; Kasual rematches
    # have no end, so the rematch is offered again
    await c1.request({"type": "result", "match": match, "outcome": "left"})
    for c in (c1, c2):
        res = await c.expect("match_result", match=match)
        assert res["status"] == "abandoned" and res["left"] == p1 and res["left_side"] == 1
        between = await c.expect("series", state="between")
        assert between["you"] == 0 and between["them"] == 0 and between["played"] == 1
    # the other game's late report changes nothing, and both can play on
    ack = await c2.request({"type": "result", "match": match, "outcome": "disconnected"})
    assert ack["type"] == "result_ack"
    gos = await next_match(server, sides, between["series"])
    assert gos[1]["match"] != match


async def test_a_game_that_left_says_how_it_ended(server, connect, udp_peers, caplog):
    """A launcher whose game ended mid-match says how (from the game's log): the server's log keeps it with the match,
    and names who left the series; junk in those fields is dropped, never refused."""
    sides, gos = await first_match(server, connect, udp_peers, ["Jarek", "Tanya"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    with caplog.at_level("INFO", logger="mk3d.server"):
        await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "left",
                          "exit": "asked to leave 2", "exit_code": 0})
        between = await c2.expect("series", state="between")
        assert (await c1.request({"type": "series_ready", "series": between["series"], "ready": False}))["type"] \
            == "series_left"
        await c2.expect("series", state="over")
    abandoned = next(r.getMessage() for r in caplog.records if "ABANDONED" in r.getMessage())
    assert "'exit': 'asked to leave 2'" in abandoned and "'exit_code': 0" in abandoned
    assert any(f"over: left after 1 matches ({p1} left)" in r.getMessage() for r in caplog.records)

    sides, gos = await first_match(server, connect, udp_peers, ["Kenshi", "Hsu"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    caplog.clear()
    with caplog.at_level("INFO", logger="mk3d.server"):
        ack = await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "left",
                                "exit": "x" * 41, "exit_code": "0"})
        assert ack["type"] == "result_ack"
        await c2.expect("series", state="between")
    abandoned = next(r.getMessage() for r in caplog.records if "ABANDONED" in r.getMessage())
    assert "'exit'" not in abandoned and "'exit_code'" not in abandoned


async def test_closing_the_game_mid_match_loses_a_ranked_set_even_at_nil(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kai", "Sareena"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    await c2.request({"type": "result", "match": gos[1]["match"], "outcome": "left"})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1
    assert over["elo"][p1]["delta"] == 16 and over["elo"][p2]["delta"] == -16


async def test_a_player_who_vanishes_mid_match_loses_the_set(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Frost", "Sektor"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 2)  # P2 leads 1-0
    gos = await next_match(server, sides, s1["series"])
    await c2.close()  # P2's launcher goes away during the next match
    await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "disconnected"})
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1


async def test_kasual_rematches_go_on_through_matches_that_do_not_count(server, connect, udp_peers):
    """Games that lose each other end the match without a result; a Kasual series offers the rematch again however
    often that happens (a ranked set would end without a rating change after series_max_no_contest of them)."""
    sides, gos = await first_match(server, connect, udp_peers, ["Jax", "Sonya"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    for _ in range(server.cfg.series_max_no_contest + 1):
        for c in (c1, c2):
            await c.request({"type": "result", "match": gos[1]["match"], "outcome": "disconnected"})
        res = await c1.expect("match_result", match=gos[1]["match"])
        assert res["status"] == "no_contest" and res["left"] is None
        between = await c1.expect("series", state="between")
        c2.take_all("series")
        gos = await next_match(server, sides, between["series"])
    assert between["played"] == server.cfg.series_max_no_contest + 1
