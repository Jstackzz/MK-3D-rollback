"""Stand-in for the launchers and the server on one PC: passes the players' spectator feeds on to spectators.

The players' games (<prefix>_NET_FEED=127.0.0.1:<players port>) connect to --players and write their feed, packets
after their u16 length (recomp/common/src/mk3d/netplay.cpp); spectators' games (<prefix>_NET_WATCH=1,
<prefix>_NET_FEED=127.0.0.1:<watchers port>) connect to --watchers and get every packet the players wrote, from the
first one. --batch holds what came for that many seconds first, as the launchers do (they send every 0.1 s).

    python tools/py/feedrelay.py --players 7150 --watchers 7151 --batch 0.1
"""
import argparse
import asyncio
import struct
import time

backlog = []      # every packet so far, length prefix included
watchers = set()  # their writers
pending = []      # packets held for the next batch
stats = {"packets": 0, "bytes": 0, "players": 0, "watchers": 0}


def publish(frames):
    backlog.extend(frames)
    for w in list(watchers):
        for f in frames:
            w.write(f)


async def player(reader, writer):
    stats["players"] += 1
    print(f"{time.strftime('%H:%M:%S')} player connected", flush=True)
    buf = b""
    while True:
        data = await reader.read(1 << 16)
        if not data:
            break
        buf += data
        frames = []
        while len(buf) >= 2:
            n = struct.unpack_from("<H", buf)[0]
            if len(buf) < 2 + n:
                break
            frames.append(buf[: 2 + n])
            buf = buf[2 + n:]
        stats["packets"] += len(frames)
        stats["bytes"] += sum(len(f) for f in frames)
        if args.batch > 0:
            pending.extend(frames)
        else:
            publish(frames)
    print(f"{time.strftime('%H:%M:%S')} player left", flush=True)
    writer.close()


async def watcher(reader, writer):
    stats["watchers"] += 1
    print(f"{time.strftime('%H:%M:%S')} spectator connected, {len(backlog)} packets to catch up", flush=True)
    for f in backlog:
        writer.write(f)
    watchers.add(writer)
    try:
        while await reader.read(1 << 16):
            pass
    finally:
        watchers.discard(writer)
        print(f"{time.strftime('%H:%M:%S')} spectator left", flush=True)


async def batches():
    while True:
        await asyncio.sleep(args.batch if args.batch > 0 else 1)
        if pending:
            frames = pending[:]
            pending.clear()
            publish(frames)


async def report():
    while True:
        await asyncio.sleep(10)
        print(f"{time.strftime('%H:%M:%S')} {stats}", flush=True)


async def main():
    p = await asyncio.start_server(player, "127.0.0.1", args.players)
    w = await asyncio.start_server(watcher, "127.0.0.1", args.watchers)
    print(f"feed relay: players on {args.players}, spectators on {args.watchers}", flush=True)
    async with p, w:
        await asyncio.gather(p.serve_forever(), w.serve_forever(), batches(), report())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=7150)
    ap.add_argument("--watchers", type=int, default=7151)
    ap.add_argument("--batch", type=float, default=0.1)
    args = ap.parse_args()
    asyncio.run(main())
