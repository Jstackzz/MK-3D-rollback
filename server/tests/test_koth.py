"""King of the Hill lobbies (docs/PROTOCOL.md 1.10.2): the winner stays on as the king (player 1 in the next match),
the loser goes to the back of the line and the first in line plays the king next; everyone else watches. After each
match its spectators rate the winner, a whole number from 1 to 10, which become the winner's Respect Points, shown on
the leaderboards."""

from __future__ import annotations

import asyncio

import pytest

from server import auth
from server.client import ServerError

from .conftest import run_to_go


def create_msg(**kw):
    return {"type": "lobby_create", "name": "Hill", "game": "mkd", "mode": "kombat", "password": "", "rounds": 3,
            "region": "eu", "character": "Scorpion", "koth": True, **kw}


async def koth_lobby(server, connect, names, **kw):
    """A King of the Hill lobby hosted by the first name, the others joining to play in that order."""
    server.cfg.lobby_countdown = 0.05
    conns = {n: await connect(n) for n in names}
    host, *rest = names
    code = (await conns[host].request(create_msg(**kw)))["lobby"]["id"]
    for n in rest:
        await conns[n].request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    return code, conns


def lineup(server, code):
    """The two players (the king first), the line in order, and who watches only."""
    lobby = server.lobbies[code]
    players = sorted((m.session.name for m in lobby.players()), key=lambda n: n != (lobby.king and lobby.king.session.name))
    return players, [m.session.name for m in lobby.line], [m.session.name for m in lobby.spectators()
                                                           if m not in lobby.line]


async def fight(server, udp_peers, conns, players, winner):
    """The two players ready, the match runs and `winner` wins it; returns both match_go by name."""
    for n in players:
        await conns[n].request({"type": "lobby_ready", "ready": True})
    gos = dict(zip(players, await run_to_go(server, [(conns[n], await udp_peers()) for n in players])))
    side = gos[winner]["side"]
    rounds = (2, 0) if side == 1 else (0, 2)
    for n in players:
        await conns[n].request({"type": "result", "match": gos[n]["match"], "winner": side, "p1_rounds": rounds[0],
                                "p2_rounds": rounds[1]})
    for n in players:
        assert (await conns[n].expect("match_result", match=gos[n]["match"]))["status"] == "agreed"
    return gos


def notes(conn):
    """The lobby's own lines in its chat so far (who is king, who is next, Respect Points)."""
    return [c["text"] for c in conn.take_all("lobby_chat") if c["from"] == ""]


async def test_the_winner_stays_on_and_the_line_rotates(server, connect, udp_peers):
    code, c = await koth_lobby(server, connect, ["Goro", "Kintaro", "Sheeva", "Motaro"])
    state = (await c["Goro"].request({"type": "lobby_list", "game": "mkd"}))["lobbies"][0]
    assert (state["koth"], state["players"], state["in_line"], state["spectators"]) == (True, 2, 2, 2)
    assert lineup(server, code) == (["Goro", "Kintaro"], ["Sheeva", "Motaro"], [])
    lobby_state = (await c["Motaro"].request({"type": "lobby_character", "character": "Kabal"}))["lobby"]
    assert lobby_state["koth"] is True and lobby_state["king"] is None and lobby_state["rating"] is None
    assert {m["name"]: (m["spectator"], m["line"], m["respect"]) for m in lobby_state["members"]} == {
        "Goro": (False, None, 0), "Kintaro": (False, None, 0), "Sheeva": (True, 1, 0), "Motaro": (True, 2, 0)}

    # the first match: the host is player 1; the guest wins and is king, the host goes to the back of the line
    gos = await fight(server, udp_peers, c, ["Goro", "Kintaro"], "Kintaro")
    assert (gos["Goro"]["side"], gos["Kintaro"]["side"]) == (1, 2)
    assert lineup(server, code) == (["Kintaro", "Sheeva"], ["Motaro", "Goro"], [])
    rating = server.lobbies[code].rating
    assert rating is not None and set(rating.raters.values()) == {"Sheeva", "Motaro"}  # the spectators, not Goro
    said = await c["Sheeva"].expect("lobby_chat", pred=lambda m: m["from"] == "")
    assert said["text"] == "Kintaro is the king. Next up: Sheeva. Spectators, rate Kintaro's win (1 to 10)."

    # the next match waits for the rating: both ready, no countdown yet
    for n in ("Kintaro", "Sheeva"):
        state = (await c[n].request({"type": "lobby_ready", "ready": True}))["lobby"]
    assert state["starts_in"] is None and state["rating"]["winner"] == "Kintaro"
    assert state["rating"]["raters"] == ["Motaro", "Sheeva"] and state["rating"]["rated"] == []
    assert 0 < state["rating"]["seconds"] <= server.cfg.respect_window
    rated = await c["Sheeva"].request({"type": "lobby_rate", "match": rating.match, "score": 7})
    assert (rated["type"], rated["score"], rated["winner"]) == ("lobby_rated", 7, "Kintaro")
    await c["Motaro"].request({"type": "lobby_rate", "match": rating.match, "score": 10})
    # every spectator rated: the rating closes, the lobby hears the result and the match counts down
    said = await c["Goro"].expect("lobby_chat", pred=lambda m: "Respect" in m["text"])
    assert said["text"] == "Kintaro gets 17 Respect Points from 2 ratings (17 in all)."
    assert server.db.respect(server.db.account_by_name("Kintaro")["id"], "mkd", "kombat") == {"points": 17,
                                                                                                "ratings": 2}

    # the king plays as player 1 and keeps the crown; the challenger goes to the back
    gos = dict(zip(["Kintaro", "Sheeva"], await run_to_go(server, [(c[n], await udp_peers())
                                                                    for n in ("Kintaro", "Sheeva")])))
    assert (gos["Kintaro"]["side"], gos["Sheeva"]["side"]) == (1, 2)
    for n in ("Kintaro", "Sheeva"):
        await c[n].request({"type": "result", "match": gos[n]["match"], "winner": 1, "p1_rounds": 2, "p2_rounds": 1})
    await c["Kintaro"].expect("match_result")
    assert lineup(server, code) == (["Kintaro", "Motaro"], ["Goro", "Sheeva"], [])
    assert server.lobbies[code].streak == 2
    said = await c["Goro"].expect("lobby_chat", pred=lambda m: "stays king" in m["text"])
    assert said["text"].startswith("Kintaro stays king: 2 wins in a row. Next up: Motaro.")
    rating = server.lobbies[code].rating
    assert set(rating.raters.values()) == {"Goro", "Motaro"}  # Goro watched this one
    await c["Goro"].request({"type": "lobby_rate", "match": rating.match, "score": 4})
    await c["Motaro"].request({"type": "lobby_rate", "match": rating.match, "score": 5})

    # the challenger beats the king: they are the new king, player 1 next
    gos = await fight(server, udp_peers, c, ["Kintaro", "Motaro"], "Motaro")
    assert (gos["Kintaro"]["side"], gos["Motaro"]["side"]) == (1, 2)
    assert lineup(server, code) == (["Motaro", "Goro"], ["Sheeva", "Kintaro"], [])
    said = await c["Sheeva"].expect("lobby_chat", pred=lambda m: "new king" in m["text"])
    assert said["text"].startswith("Motaro beat the king Kintaro: Motaro is the new king. Next up: Goro.")
    kintaro = server.db.account_by_name("Kintaro")["id"]
    assert server.db.respect(kintaro, "mkd", "kombat") == {"points": 26, "ratings": 4}

    # lobby matches count as Kasual: the records moved, Elo did not
    kombat = (await c["Kintaro"].request({"type": "profile"}))["stats"]["mkd"]["kombat"]
    assert kombat["casual"] == {"wins": 2, "losses": 1}
    assert kombat["respect"] == {"points": 26, "ratings": 4}
    assert kombat["ranked"]["wins"] + kombat["ranked"]["losses"] == 0


async def test_rating_rules(server, connect, udp_peers):
    code, c = await koth_lobby(server, connect, ["Liu", "Kung", "Jade", "Kitana"])
    await c["Kitana"].request({"type": "lobby_role", "role": "spectator"})  # watches only, out of the line
    assert lineup(server, code) == (["Liu", "Kung"], ["Jade"], ["Kitana"])
    await fight(server, udp_peers, c, ["Liu", "Kung"], "Liu")
    match = server.lobbies[code].rating.match
    assert set(server.lobbies[code].rating.raters.values()) == {"Jade", "Kitana"}  # every spectator, in line or not

    async def refused(name, code_, **msg):
        with pytest.raises(ServerError) as exc:
            await c[name].request({"type": "lobby_rate", "match": match, **msg})
        assert exc.value.code == code_

    # whole numbers from 1 to 10 only
    for bad in (0, 11, 7.5, "7", True, None):
        await refused("Jade", "bad_request", score=bad)
    await refused("Liu", "not_rater", score=10)  # the winner
    await refused("Kung", "not_rater", score=1)  # the loser
    with pytest.raises(ServerError) as exc:
        await c["Jade"].request({"type": "lobby_rate", "match": "000000000000", "score": 5})
    assert exc.value.code == "rating_closed"
    await c["Jade"].request({"type": "lobby_rate", "match": match, "score": 9})
    await refused("Jade", "already_rated", score=9)
    assert server.lobbies[code].rating is not None  # Kitana has not rated yet
    # a spectator who leaves cannot rate any more: the rest have, so the rating closes
    await c["Kitana"].request({"type": "lobby_leave"})
    await asyncio.sleep(0.1)
    assert server.lobbies[code].rating is None
    liu = server.db.account_by_name("Liu")["id"]
    assert server.db.respect(liu, "mkd", "kombat") == {"points": 9, "ratings": 1}
    assert [(r["score"], r["winner"]) for r in server.db.ratings(match)] == [(9, liu)]
    await refused("Jade", "rating_closed", score=9)


async def test_rating_time_runs_out(server, connect, udp_peers):
    server.cfg.respect_window = 0.4
    code, c = await koth_lobby(server, connect, ["Raiden", "Shinnok", "Fujin"])
    await fight(server, udp_peers, c, ["Raiden", "Shinnok"], "Shinnok")
    match = server.lobbies[code].rating.match
    for n in ("Shinnok", "Fujin"):  # the king and the next in line ready while the rating runs
        await c[n].request({"type": "lobby_ready", "ready": True})
    assert server.lobbies[code].starts_at is None
    await asyncio.sleep(0.7)  # nobody rated in time: closed, and the match counts down
    assert server.lobbies[code].rating is None
    with pytest.raises(ServerError) as exc:
        await c["Raiden"].request({"type": "lobby_rate", "match": match, "score": 5})
    assert exc.value.code in ("rating_closed", "not_rater")
    found = [await c[n].expect("match_found", timeout=3) for n in ("Shinnok", "Fujin")]
    assert [f["side"] for f in found] == [1, 2]  # the king is player 1
    assert not [t for t in notes(c["Raiden"]) if "Respect" in t]  # no ratings, no Respect line


async def test_two_players_play_on_and_the_winner_is_player_one(server, connect, udp_peers):
    """Nobody waits: the loser plays the king again, the king as player 1 (the host too)."""
    code, c = await koth_lobby(server, connect, ["Sektor", "Cyrax"])
    await fight(server, udp_peers, c, ["Sektor", "Cyrax"], "Cyrax")
    assert lineup(server, code) == (["Cyrax", "Sektor"], [], [])
    assert server.lobbies[code].rating is None  # nobody watched it
    gos = await fight(server, udp_peers, c, ["Sektor", "Cyrax"], "Sektor")
    assert (gos["Cyrax"]["side"], gos["Sektor"]["side"]) == (1, 2)
    assert server.lobbies[code].king.session.name == "Sektor"
    # someone comes in: they wait in line (both places taken) and play the king next
    smoke = await connect("Smoke")
    joined = await smoke.request({"type": "lobby_join", "lobby": code, "character": "Smoke"})
    me = next(m for m in joined["lobby"]["members"] if m["name"] == "Smoke")
    assert (me["spectator"], me["line"]) == (True, 1)
    c["Smoke"] = smoke
    await fight(server, udp_peers, c, ["Sektor", "Cyrax"], "Sektor")
    assert lineup(server, code) == (["Sektor", "Smoke"], ["Cyrax"], [])


async def test_one_who_does_not_ready_goes_to_the_back(server, connect, udp_peers):
    server.cfg.koth_ready_timeout = 0.5
    code, c = await koth_lobby(server, connect, ["Sindel", "Jade", "Tanya", "Mileena"])
    assert server.lobbies[code].ready_by is not None  # someone waits: the two must ready
    await c["Jade"].request({"type": "lobby_ready", "ready": True})
    await asyncio.sleep(0.8)
    # the host never readied: the first in line plays instead, the one who was ready stays
    assert lineup(server, code)[1:] == (["Mileena", "Sindel"], [])
    assert sorted(lineup(server, code)[0]) == ["Jade", "Tanya"]
    said = [t for t in notes(c["Jade"]) if "in time" in t]
    assert said == ["Sindel did not ready in time: to the back of the line. Next up: Tanya."]
    # a king who does not ready loses the crown
    server.cfg.koth_ready_timeout = 30
    await fight(server, udp_peers, c, ["Jade", "Tanya"], "Tanya")
    lobby = server.lobbies[code]
    lobby.rating = None  # nobody needs to rate here
    assert lobby.king.session.name == "Tanya" and lineup(server, code)[0] == ["Tanya", "Mileena"]
    server.cfg.koth_ready_timeout = 0.3
    lobby.ready_by = None
    await c["Mileena"].request({"type": "lobby_ready", "ready": True})
    await asyncio.sleep(0.6)
    assert lobby.king is None and lobby.streak == 0
    assert sorted(lineup(server, code)[0]) == ["Mileena", "Sindel"] and lineup(server, code)[1] == ["Jade", "Tanya"]


async def test_leaving_and_roles_move_the_line(server, connect, udp_peers):
    code, c = await koth_lobby(server, connect, ["Kabal", "Mavado", "Hsu", "Kira"])
    # a player who leaves: the first in line takes the place at once
    await c["Mavado"].request({"type": "lobby_leave"})
    assert lineup(server, code) == (["Kabal", "Hsu"], ["Kira"], [])
    said = await c["Kabal"].expect("lobby_chat", pred=lambda m: m["from"] == "")
    assert said["text"] == "Mavado left. Next up: Hsu."
    # out of the line to watch only, then back in at its end
    await c["Kira"].request({"type": "lobby_role", "role": "spectator"})
    assert lineup(server, code) == (["Kabal", "Hsu"], [], ["Kira"])
    await c["Kira"].request({"type": "lobby_role", "role": "player"})
    assert lineup(server, code) == (["Kabal", "Hsu"], ["Kira"], [])
    # a player who steps aside watches only; the first in line plays
    await c["Kabal"].request({"type": "lobby_role", "role": "spectator"})
    assert lineup(server, code) == (["Hsu", "Kira"], [], ["Kabal"])
    # with a place free and nobody waiting, getting in line plays at once
    await c["Hsu"].request({"type": "lobby_leave"})
    assert lineup(server, code) == (["Kira"], [], ["Kabal"])
    await c["Kabal"].request({"type": "lobby_role", "role": "player"})
    assert lineup(server, code) == (["Kabal", "Kira"], [], []) or lineup(server, code) == (["Kira", "Kabal"], [], [])
    # a spectator may pick a fighter for their turn while a match runs; a player may not
    late = await connect("Ermac")
    await late.request({"type": "lobby_join", "lobby": code, "character": "Ermac"})
    for n in ("Kabal", "Kira"):
        await c[n].request({"type": "lobby_ready", "ready": True})
    await c["Kabal"].expect("match_found")
    state = (await late.request({"type": "lobby_character", "character": "Reptile"}))["lobby"]
    assert next(m["character"] for m in state["members"] if m["name"] == "Ermac") == "Reptile"
    with pytest.raises(ServerError) as exc:
        await c["Kabal"].request({"type": "lobby_character", "character": "Reptile"})
    assert exc.value.code == "busy"


async def test_a_player_whose_game_closes_loses_and_nobody_rates_it(server, connect, udp_peers):
    code, c = await koth_lobby(server, connect, ["Nitara", "Ashrah", "Havik"])
    for n in ("Nitara", "Ashrah"):
        await c[n].request({"type": "lobby_ready", "ready": True})
    gos = dict(zip(["Nitara", "Ashrah"], await run_to_go(server, [(c[n], await udp_peers())
                                                                  for n in ("Nitara", "Ashrah")])))
    await c["Nitara"].request({"type": "result", "match": gos["Nitara"]["match"], "outcome": "left"})
    assert (await c["Ashrah"].expect("match_result"))["status"] == "abandoned"
    assert lineup(server, code) == (["Ashrah", "Havik"], ["Nitara"], [])
    assert server.lobbies[code].rating is None


async def test_a_dropped_player_is_back_at_the_end_of_the_line(server, connect):
    """A player whose connection drops gives their place to the first in line at once (a king the crown); back within
    the lobby's grace, they wait at the end of the line."""
    code, c = await koth_lobby(server, connect, ["Jarek", "Kobra", "Kira"])
    lobby = server.lobbies[code]
    lobby.king = next(m for m in lobby.members if m.session.name == "Jarek")
    token = c["Jarek"].account["token"]
    await c["Jarek"].close()
    await c["Kobra"].expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)
    assert lineup(server, code) == (["Kobra", "Kira"], [], []) or lineup(server, code) == (["Kira", "Kobra"], [], [])
    assert lobby.king is None
    back = await connect()
    await back.request({"type": "auth", "token": token})
    state = (await back.expect("lobby"))["lobby"]
    me = next(m for m in state["members"] if m["name"] == "Jarek")
    assert (me["spectator"], me["line"]) == (True, 1)


async def test_online_practice_has_no_king_of_the_hill(server, connect):
    """Online Practice (1.0.7) is for two players with nobody watching: no line, nobody to rate."""
    host = await connect("Shujinko")
    with pytest.raises(ServerError) as exc:
        await host.request(create_msg(mode="sparring"))
    assert exc.value.code == "bad_mode"
    lobby = (await host.request(create_msg(mode="sparring", koth=False)))["lobby"]
    assert (lobby["koth"], lobby["max_spectators"]) == (False, 0)
    assert "king" not in lobby and "line" not in lobby["members"][0]


async def test_launchers_without_king_of_the_hill_only_watch(server, connect):
    old = await connect("BoRaiCho", features=())
    with pytest.raises(ServerError) as exc:
        await old.request(create_msg())
    assert exc.value.code == "update_needed"
    code, c = await koth_lobby(server, connect, ["Kenshi", "LiMei"])
    with pytest.raises(ServerError) as exc:
        await old.request({"type": "lobby_join", "lobby": code})
    assert exc.value.code == "update_needed"
    await old.request({"type": "lobby_join", "lobby": code, "spectate": True})
    with pytest.raises(ServerError) as exc:
        await old.request({"type": "lobby_role", "role": "player"})
    assert exc.value.code == "update_needed"
    assert lineup(server, code) == (["Kenshi", "LiMei"], [], ["BoRaiCho"])
    assert "koth" in old.welcome["features"]  # the server says it has them
    # a plain lobby is as it was
    await old.request({"type": "lobby_leave"})
    plain = await old.request(create_msg(koth=False))
    assert plain["lobby"]["koth"] is False and "king" not in plain["lobby"]
    assert "line" not in plain["lobby"]["members"][0]


async def test_respect_points_on_the_leaderboards(server, connect):
    db = server.db
    ids = {n: db.create_account(n, "x") for n in ("Scorpion", "SubZero", "Smoke", "Noob", "Rain")}
    db.record_ranked_set(ids["Scorpion"], ids["SubZero"], "mkd", "kombat")
    for i, (winner, score) in enumerate([("SubZero", 9), ("SubZero", 8), ("Smoke", 10), ("Smoke", 7),
                                         ("Noob", 10), ("Noob", 7), ("Rain", 4)]):
        assert db.add_rating(f"m{i}", ids["Rain"] if winner != "Rain" else ids["Noob"], ids[winner], "mkd", "kombat",
                             score)
    assert not db.add_rating("m0", ids["Rain"], ids["SubZero"], "mkd", "kombat", 10)  # once per match and spectator
    db.add_rating("x1", ids["Rain"], ids["Scorpion"], "mka", "kombat", 10)  # another game's: not on this board

    conn = await connect("Kano")
    board = await conn.request({"type": "leaderboard", "game": "mkd", "mode": "kombat", "by": "respect"})
    assert board["by"] == "respect" and board["total"] == 4
    # the most first; the same from fewer ratings before; then by name
    assert [(r["rank"], r["name"], r["respect"], r["ratings"]) for r in board["rows"]] == [
        (1, "Noob", 17, 2), (2, "Smoke", 17, 2), (3, "SubZero", 17, 2), (4, "Rain", 4, 1)]
    sub = board["rows"][2]
    assert (sub["elo"], sub["wins"], sub["losses"]) == (db.rating(ids["SubZero"], "mkd", "kombat")["elo"], 0, 1)
    rain = board["rows"][3]
    assert (rain["title"], rain["wins"], rain["losses"]) == ("Beginner", 0, 0)  # never played ranked: the start

    # by Elo (the default): the ranked players, each with their Respect Points
    elo_board = await conn.request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert elo_board["by"] == "elo"
    assert [(r["name"], r["respect"]) for r in elo_board["rows"]] == [("Scorpion", 0), ("SubZero", 17)]
    with pytest.raises(ServerError) as exc:
        await conn.request({"type": "leaderboard", "game": "mkd", "mode": "kombat", "by": "wins"})
    assert exc.value.code == "bad_request"

    # "me": where the asking player stands on that board (Smoke signs in with a saved login)
    token = "ab" * 16
    db.add_token(ids["Smoke"], auth.token_hash(token), 4e9)
    smoke = await connect()
    await smoke.request({"type": "auth", "token": token})
    mine = await smoke.request({"type": "leaderboard", "game": "mkd", "mode": "kombat", "by": "respect"})
    assert (mine["me"]["rank"], mine["me"]["respect"], mine["me"]["ratings"]) == (2, 17, 2)
    assert "me" not in await smoke.request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    profile = await smoke.request({"type": "profile"})
    assert profile["stats"]["mkd"]["kombat"]["respect"] == {"points": 17, "ratings": 2}
    assert "respect" not in profile["stats"]["mka"]["kombat"]
