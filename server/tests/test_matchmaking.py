"""Pairing rules (pure functions) plus queue integration over the wire."""

from __future__ import annotations

import pytest

from server.client import ServerError
from server.matchmaking import MatchmakingConfig, PingCache, QueueEntry, elo_window, find_pairs, max_ping_now

from .conftest import queue_msg

CFG = MatchmakingConfig()


def entry(account_id: int, since: float = 0.0, *, game: str = "mkd", mode: str = "kombat", ranked: bool = False,
          region: str = "eu", elo: int = 1500, max_ping: int = 0, strict: bool = False,
          ip: str | None = None, death_traps: bool = True) -> QueueEntry:
    return QueueEntry(account_id=account_id, name=f"p{account_id}", game=game, mode=mode, ranked=ranked,
                      region=region, max_ping=max_ping, strict=strict, character="Scorpion", delay=2, since=since,
                      elo=elo, public_ip=ip or f"10.0.0.{account_id}", death_traps=death_traps)


def ids(pairs):
    return [tuple(sorted((a.account_id, b.account_id))) for a, b in pairs]


def test_casual_first_come_first_served():
    entries = [entry(3, since=2), entry(1, since=0), entry(2, since=1)]
    assert ids(find_pairs(entries, 5, CFG, PingCache())) == [(1, 2)]
    four = entries + [entry(4, since=3)]
    assert ids(find_pairs(four, 5, CFG, PingCache())) == [(1, 2), (3, 4)]


def test_queues_do_not_mix():
    entries = [entry(1), entry(2, mode="chess"), entry(3, ranked=True), entry(4, game="mka")]
    assert find_pairs(entries, 100, CFG, PingCache()) == []


def test_casual_death_traps_choices_do_not_mix():
    entries = [entry(1, since=0, death_traps=False), entry(2, since=1), entry(3, since=2, death_traps=False),
               entry(4, since=3)]
    assert ids(find_pairs(entries, 5, CFG, PingCache())) == [(1, 3), (2, 4)]
    assert find_pairs(entries[:2], 1000, CFG, PingCache()) == []  # waiting longer changes nothing
    assert [e.queue_name for e in entries[:2]] == ["mkd/kombat/casual/no-traps", "mkd/kombat/casual"]
    # ranked never has them, whatever the search asked for
    ranked = [entry(5, ranked=True), entry(6, ranked=True, death_traps=False)]
    assert ids(find_pairs(ranked, 1, CFG, PingCache())) == [(5, 6)]
    assert [e.traps for e in ranked] == [False, False]


def test_region_widens_unless_strict():
    a, b = entry(1, since=0, region="eu"), entry(2, since=0, region="na")
    assert find_pairs([a, b], 5, CFG, PingCache()) == []
    assert ids(find_pairs([a, b], CFG.region_widen_after + 1, CFG, PingCache())) == [(1, 2)]
    a.strict = True
    assert find_pairs([a, b], 1000, CFG, PingCache()) == []


def test_ranked_elo_window_widens_over_time():
    a, b = entry(1, since=0, ranked=True, elo=1500), entry(2, since=0, ranked=True, elo=1700)
    assert elo_window(a, 0, CFG) == 100
    assert elo_window(a, 25, CFG) == 200
    assert elo_window(a, 10_000, CFG) == CFG.elo_max
    assert find_pairs([a, b], 5, CFG, PingCache()) == []
    assert ids(find_pairs([a, b], 20, CFG, PingCache())) == [(1, 2)]
    b.strict = True  # strict keeps the base window for that player
    assert find_pairs([a, b], 1000, CFG, PingCache()) == []


def test_ranked_prefers_closest_rating():
    entries = [entry(1, since=0, ranked=True, elo=1500), entry(2, since=1, ranked=True, elo=1590),
               entry(3, since=2, ranked=True, elo=1510)]
    assert ids(find_pairs(entries, 3, CFG, PingCache())) == [(1, 3)]


def test_ranked_refuses_same_ip_unless_allowed():
    a = entry(1, ranked=True, ip="203.0.113.5")
    b = entry(2, ranked=True, ip="203.0.113.5")
    assert find_pairs([a, b], 1, CFG, PingCache()) == []
    assert ids(find_pairs([a, b], 1, MatchmakingConfig(allow_same_ip=True), PingCache())) == [(1, 2)]
    casual = [entry(3, ip="203.0.113.5"), entry(4, ip="203.0.113.5")]
    assert ids(find_pairs(casual, 1, CFG, PingCache())) == [(3, 4)]


def test_measured_ping_cache_blocks_bad_pairs():
    cache = PingCache(ttl=60)
    a, b = entry(1, max_ping=100), entry(2, max_ping=0)
    cache.put(1, 2, 180, now=0)
    assert find_pairs([a, b], 1, CFG, cache) == []  # measured 180 ms > 100 ms limit
    cache.put(1, 2, 90, now=0)
    assert ids(find_pairs([a, b], 1, CFG, cache)) == [(1, 2)]
    cache.put_unreachable(1, 2, now=0)
    assert find_pairs([a, b], 1, CFG, cache) == []
    assert ids(find_pairs([a, b], 61, CFG, cache)) == [(1, 2)]  # entry expired
    c = entry(3)
    cache.put_unreachable(1, 2, now=100)
    assert ids(find_pairs([a, b, c], 101, CFG, cache)) == [(1, 3)]  # 1 skips 2 and takes 3



def test_relayed_matches_are_estimated_from_each_players_way_to_the_server():
    # through the relay a match's round trip is each player's to the server: a pair over a limit is not offered
    a, b = entry(1, max_ping=150), entry(2)
    a.server_rtt, b.server_rtt = 90, 80
    assert find_pairs([a, b], 1, CFG, PingCache(), relayed=True) == []
    assert ids(find_pairs([a, b], 1, CFG, PingCache())) == [(1, 2)]  # direct: only a measurement can say
    b.server_rtt = 40
    assert ids(find_pairs([a, b], 1, CFG, PingCache(), relayed=True)) == [(1, 2)]
    b.server_rtt = None  # not measured yet: offered, the match measures it
    assert ids(find_pairs([a, b], 1, CFG, PingCache(), relayed=True)) == [(1, 2)]
    b.server_rtt, b.relay = 80, True  # one of them hides their address: that match goes through the server too
    assert find_pairs([a, b], 1, CFG, PingCache()) == []


def test_a_long_search_raises_its_max_ping():
    a = entry(1, max_ping=60)
    assert [max_ping_now(a, t, CFG) for t in (0, 29.9, 30, 44.9, 45, 60, 600)] == [60, 60, 85, 85, 110, 135, 250]
    assert max_ping_now(entry(2, max_ping=60, strict=True), 600, CFG) == 60  # strict keeps it
    assert max_ping_now(entry(3, max_ping=0), 600, CFG) == 0  # any stays any
    assert max_ping_now(entry(4, max_ping=300), 600, CFG) == 300  # never below the player's own
    # a pair measured at 100 ms meets once the 60 ms search has waited long enough for 110
    cache = PingCache(ttl=600)
    b = entry(5)
    cache.put(1, 5, 100, now=0)
    assert find_pairs([a, b], 44, CFG, cache) == []
    assert ids(find_pairs([a, b], 45, CFG, cache)) == [(1, 5)]

# --- over the wire --------------------------------------------------------------------------------


async def test_casual_queue_pairs_two_players(server, connect):
    a = await connect("Liu")
    b = await connect("Kung")
    queued = await a.request(queue_msg(character="LiuKang"))
    assert queued["type"] == "queued" and queued["in_queue"] == 1
    await b.request(queue_msg(character="KungLao"))
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    assert fa["match"] == fb["match"]
    assert {fa["side"], fb["side"]} == {1, 2}
    assert fa["accept_required"] and fa["accept_timeout"] == server.cfg.accept_timeout
    assert fa["opponent"]["name"] == "Kung" and fa["opponent"]["character"] == "KungLao"
    assert fb["opponent"]["name"] == "Liu"
    assert server.queue == {}


async def test_ranked_hides_opponent_and_keeps_queues_apart(server, connect):
    a = await connect("Sonya")
    b = await connect("Jax")
    c = await connect("Kano")
    await a.request(queue_msg(ranked=True))
    await c.request(queue_msg(ranked=False))  # casual: must not pair with the ranked players
    await b.request(queue_msg(ranked=True))
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    assert fa["match"] == fb["match"] and fa["ranked"] and fa["opponent"] is None
    assert list(server.queue) == [server.db.account_by_name("Kano")["id"]]


async def test_queue_validation_and_leave(server, connect):
    a = await connect("Mileena")
    for bad, code in ((queue_msg(game="mk9"), "bad_game"), (queue_msg(mode="motor"), "bad_mode"),
                      (queue_msg(region="mars"), "bad_region")):
        with pytest.raises(ServerError) as exc:
            await a.request(bad)
        assert exc.value.code == code
    await a.request(queue_msg(game="mka", mode="motor"))
    left = await a.request({"type": "queue_leave"})
    assert left["reason"] == "left" and server.queue == {}


async def test_decline_requeues_only_the_other_player(server, connect, udp_peers):
    a = await connect("Kitana")
    b = await connect("Jade")
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    a.send({"type": "match_decline", "match": fa["match"]})
    ca, cb = await a.expect("match_cancelled"), await b.expect("match_cancelled")
    assert ca["reason"] == cb["reason"] == "declined"
    assert ca["requeued"] is False and cb["requeued"] is True
    assert (await a.expect("queue_left"))["reason"] == "removed"
    jade_id = server.db.account_by_name("Jade")["id"]
    assert list(server.queue) == [jade_id]


async def test_accept_window_timeout(server, connect, udp_peers):
    server.cfg.accept_timeout = 0.3
    a = await connect("Baraka")
    b = await connect("Tanya")
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa = await a.expect("match_found")
    await b.expect("match_found")
    a.send({"type": "match_accept", "match": fa["match"]})
    ca, cb = await a.expect("match_cancelled", timeout=3), await b.expect("match_cancelled", timeout=3)
    assert ca["reason"] == "accept_timeout"
    assert ca["requeued"] is True and cb["requeued"] is False



async def test_match_found_tells_each_player_the_others_connection(server, connect):
    a, b = await connect("Shujinko"), await connect("Onaga")
    queued = await a.request({**queue_msg(max_ping=80), "link": "wifi", "server_rtt": 30})
    assert queued["ping_widen"] == {"after": 30.0, "every": 15.0, "step": 25, "cap": 250}
    await b.request({**queue_msg(), "link": "ethernet", "server_rtt": 45, "relay": True})
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    # B hides its address, so the match goes through the server: its ping is both ways to it
    assert fa["connection"] == {"link": "ethernet", "ping": 45, "estimate": 75, "measured": False}
    assert fb["connection"] == {"link": "wifi", "ping": 30, "estimate": 75, "measured": False}
