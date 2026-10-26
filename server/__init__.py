"""MK3D Online lobby server: accounts, matchmaking, lobbies, leaderboards and UDP rendezvous.

One asyncio process, standard library only (sqlite3, ssl, hashlib). The wire protocol is
specified in docs/PROTOCOL.md.
"""

__version__ = "0.1.0"
