"""SQLite storage: accounts, login tokens, per game/mode stats, match log and result reports."""

from __future__ import annotations

import sqlite3
import time
from collections.abc import Iterator
from contextlib import contextmanager
from typing import Any

from . import elo

SCHEMA_VERSION = 1

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS accounts (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL UNIQUE COLLATE NOCASE,
    pw_hash    TEXT NOT NULL,
    created    REAL NOT NULL,
    last_login REAL
);
CREATE TABLE IF NOT EXISTS tokens (
    token_hash TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created    REAL NOT NULL,
    expires    REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS stats (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    game       TEXT NOT NULL,
    mode       TEXT NOT NULL,
    kind       TEXT NOT NULL CHECK (kind IN ('ranked', 'casual')),
    elo        INTEGER NOT NULL DEFAULT 1500,
    wins       INTEGER NOT NULL DEFAULT 0,
    losses     INTEGER NOT NULL DEFAULT 0,
    updated    REAL,
    PRIMARY KEY (account_id, game, mode, kind)
);
CREATE INDEX IF NOT EXISTS stats_board ON stats (game, mode, kind, elo DESC);
CREATE TABLE IF NOT EXISTS matches (
    id            TEXT PRIMARY KEY,
    game          TEXT NOT NULL,
    mode          TEXT NOT NULL,
    ranked        INTEGER NOT NULL,
    lobby         TEXT,
    p1            INTEGER NOT NULL,
    p2            INTEGER NOT NULL,
    created       REAL NOT NULL,
    started       REAL,
    ended         REAL,
    status        TEXT NOT NULL,
    cancel_reason TEXT,
    seed          INTEGER,
    rtt_ms        INTEGER,
    winner        INTEGER,
    p1_rounds     INTEGER,
    p2_rounds     INTEGER,
    p1_elo_before INTEGER,
    p1_elo_after  INTEGER,
    p2_elo_before INTEGER,
    p2_elo_after  INTEGER
);
CREATE TABLE IF NOT EXISTS reports (
    match_id   TEXT NOT NULL,
    account_id INTEGER NOT NULL,
    side       INTEGER NOT NULL,
    winner     INTEGER,
    p1_rounds  INTEGER,
    p2_rounds  INTEGER,
    outcome    TEXT,
    frame      INTEGER,
    received   REAL NOT NULL,
    PRIMARY KEY (match_id, account_id)
);
"""

MATCH_FIELDS = {
    "started", "ended", "status", "cancel_reason", "seed", "rtt_ms", "winner", "p1_rounds", "p2_rounds",
    "p1_elo_before", "p1_elo_after", "p2_elo_before", "p2_elo_after",
}


class NameTaken(Exception):
    pass


class Database:
    def __init__(self, path: str) -> None:
        self.path = path
        self.conn = sqlite3.connect(path, isolation_level=None)  # autocommit; explicit transactions below
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA foreign_keys = ON")
        if path != ":memory:":
            self.conn.execute("PRAGMA journal_mode = WAL")
            self.conn.execute("PRAGMA synchronous = NORMAL")
        self.conn.executescript(SCHEMA)
        self.conn.execute(
            "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', ?)", (str(SCHEMA_VERSION),)
        )

    def close(self) -> None:
        self.conn.close()

    @contextmanager
    def transaction(self) -> Iterator[sqlite3.Connection]:
        self.conn.execute("BEGIN IMMEDIATE")
        try:
            yield self.conn
        except BaseException:
            self.conn.execute("ROLLBACK")
            raise
        else:
            self.conn.execute("COMMIT")

    # --- accounts -------------------------------------------------------------------------------

    def create_account(self, name: str, pw_hash: str, now: float | None = None) -> int:
        try:
            cur = self.conn.execute(
                "INSERT INTO accounts (name, pw_hash, created) VALUES (?, ?, ?)",
                (name, pw_hash, now or time.time()),
            )
        except sqlite3.IntegrityError as exc:
            raise NameTaken(name) from exc
        return int(cur.lastrowid)

    def account_by_name(self, name: str) -> sqlite3.Row | None:
        return self.conn.execute("SELECT * FROM accounts WHERE name = ?", (name,)).fetchone()

    def account(self, account_id: int) -> sqlite3.Row | None:
        return self.conn.execute("SELECT * FROM accounts WHERE id = ?", (account_id,)).fetchone()

    def touch_login(self, account_id: int, now: float | None = None) -> None:
        self.conn.execute("UPDATE accounts SET last_login = ? WHERE id = ?", (now or time.time(), account_id))

    # --- tokens ---------------------------------------------------------------------------------

    def add_token(self, account_id: int, token_hash: str, expires: float, now: float | None = None) -> None:
        now = now or time.time()
        self.conn.execute("DELETE FROM tokens WHERE expires < ?", (now,))
        self.conn.execute(
            "INSERT INTO tokens (token_hash, account_id, created, expires) VALUES (?, ?, ?, ?)",
            (token_hash, account_id, now, expires),
        )

    def account_for_token(self, token_hash: str, now: float | None = None) -> tuple[sqlite3.Row, float] | None:
        row = self.conn.execute(
            "SELECT a.*, t.expires AS token_expires FROM tokens t JOIN accounts a ON a.id = t.account_id "
            "WHERE t.token_hash = ? AND t.expires > ?",
            (token_hash, now or time.time()),
        ).fetchone()
        if row is None:
            return None
        return row, float(row["token_expires"])

    def delete_token(self, token_hash: str) -> None:
        self.conn.execute("DELETE FROM tokens WHERE token_hash = ?", (token_hash,))

    # --- stats ----------------------------------------------------------------------------------

    def stats(self, account_id: int) -> dict[str, dict[str, dict[str, dict[str, int]]]]:
        out: dict[str, dict[str, dict[str, dict[str, int]]]] = {}
        for row in self.conn.execute(
            "SELECT game, mode, kind, elo, wins, losses FROM stats WHERE account_id = ? ORDER BY game, mode, kind",
            (account_id,),
        ):
            entry: dict[str, int] = {"wins": row["wins"], "losses": row["losses"]}
            if row["kind"] == "ranked":
                entry = {"elo": row["elo"], **entry}
            out.setdefault(row["game"], {}).setdefault(row["mode"], {})[row["kind"]] = entry
        return out

    def rating(self, account_id: int, game: str, mode: str) -> dict[str, int]:
        row = self.conn.execute(
            "SELECT elo, wins, losses FROM stats WHERE account_id = ? AND game = ? AND mode = ? AND kind = 'ranked'",
            (account_id, game, mode),
        ).fetchone()
        if row is None:
            return {"elo": elo.INITIAL, "wins": 0, "losses": 0}
        return {"elo": row["elo"], "wins": row["wins"], "losses": row["losses"]}

    def _bump(self, conn: sqlite3.Connection, account_id: int, game: str, mode: str, kind: str,
              won: bool, new_elo: int | None, now: float) -> None:
        conn.execute(
            "INSERT OR IGNORE INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
            "VALUES (?, ?, ?, ?, ?, 0, 0, ?)",
            (account_id, game, mode, kind, elo.INITIAL, now),
        )
        conn.execute(
            "UPDATE stats SET wins = wins + ?, losses = losses + ?, elo = COALESCE(?, elo), updated = ? "
            "WHERE account_id = ? AND game = ? AND mode = ? AND kind = ?",
            (1 if won else 0, 0 if won else 1, new_elo, now, account_id, game, mode, kind),
        )

    def record_casual(self, winner_id: int, loser_id: int, game: str, mode: str, now: float | None = None) -> None:
        now = now or time.time()
        with self.transaction() as conn:
            self._bump(conn, winner_id, game, mode, "casual", True, None, now)
            self._bump(conn, loser_id, game, mode, "casual", False, None, now)

    def record_ranked(self, winner_id: int, loser_id: int, game: str, mode: str,
                      now: float | None = None) -> dict[int, tuple[int, int]]:
        """Apply one ranked win; returns {account_id: (elo_before, elo_after)}."""
        now = now or time.time()
        with self.transaction() as conn:
            before_w = self.rating(winner_id, game, mode)["elo"]
            before_l = self.rating(loser_id, game, mode)["elo"]
            after_w, after_l = elo.update(before_w, before_l, 1.0)
            self._bump(conn, winner_id, game, mode, "ranked", True, after_w, now)
            self._bump(conn, loser_id, game, mode, "ranked", False, after_l, now)
        return {winner_id: (before_w, after_w), loser_id: (before_l, after_l)}

    def leaderboard(self, game: str, mode: str, limit: int, offset: int) -> tuple[list[dict[str, Any]], int]:
        total = self.conn.execute(
            "SELECT COUNT(*) FROM stats WHERE game = ? AND mode = ? AND kind = 'ranked' AND wins + losses > 0",
            (game, mode),
        ).fetchone()[0]
        rows = []
        for i, row in enumerate(
            self.conn.execute(
                "SELECT a.name, s.elo, s.wins, s.losses FROM stats s JOIN accounts a ON a.id = s.account_id "
                "WHERE s.game = ? AND s.mode = ? AND s.kind = 'ranked' AND s.wins + s.losses > 0 "
                "ORDER BY s.elo DESC, s.wins DESC, a.name COLLATE NOCASE ASC LIMIT ? OFFSET ?",
                (game, mode, limit, offset),
            )
        ):
            rows.append({"rank": offset + i + 1, "name": row["name"], "elo": row["elo"],
                         "wins": row["wins"], "losses": row["losses"]})
        return rows, int(total)

    def rank_of(self, account_id: int, game: str, mode: str) -> dict[str, int] | None:
        row = self.conn.execute(
            "SELECT s.elo, s.wins, s.losses, a.name FROM stats s JOIN accounts a ON a.id = s.account_id "
            "WHERE s.account_id = ? AND s.game = ? AND s.mode = ? AND s.kind = 'ranked' AND s.wins + s.losses > 0",
            (account_id, game, mode),
        ).fetchone()
        if row is None:
            return None
        better = self.conn.execute(
            "SELECT COUNT(*) FROM stats s JOIN accounts a ON a.id = s.account_id "
            "WHERE s.game = ? AND s.mode = ? AND s.kind = 'ranked' AND s.wins + s.losses > 0 AND "
            "(s.elo > ? OR (s.elo = ? AND s.wins > ?) OR "
            " (s.elo = ? AND s.wins = ? AND a.name < ? COLLATE NOCASE))",
            (game, mode, row["elo"], row["elo"], row["wins"], row["elo"], row["wins"], row["name"]),
        ).fetchone()[0]
        return {"rank": int(better) + 1, "elo": row["elo"], "wins": row["wins"], "losses": row["losses"]}

    # --- match log ------------------------------------------------------------------------------

    def create_match(self, match_id: str, game: str, mode: str, ranked: bool, lobby: str | None,
                     p1: int, p2: int, now: float) -> None:
        self.conn.execute(
            "INSERT INTO matches (id, game, mode, ranked, lobby, p1, p2, created, status) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'found')",
            (match_id, game, mode, int(ranked), lobby, p1, p2, now),
        )

    def update_match(self, match_id: str, **fields: Any) -> None:
        unknown = set(fields) - MATCH_FIELDS
        if unknown:
            raise ValueError(f"unknown match fields {unknown}")
        if not fields:
            return
        names = ", ".join(f"{k} = ?" for k in fields)
        self.conn.execute(f"UPDATE matches SET {names} WHERE id = ?", (*fields.values(), match_id))

    def match_row(self, match_id: str) -> sqlite3.Row | None:
        return self.conn.execute("SELECT * FROM matches WHERE id = ?", (match_id,)).fetchone()

    def add_report(self, match_id: str, account_id: int, side: int, report: dict[str, Any], now: float) -> None:
        self.conn.execute(
            "INSERT OR REPLACE INTO reports (match_id, account_id, side, winner, p1_rounds, p2_rounds, outcome, "
            "frame, received) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (match_id, account_id, side, report.get("winner"), report.get("p1_rounds"), report.get("p2_rounds"),
             report.get("outcome"), report.get("frame"), now),
        )

    def reports(self, match_id: str) -> list[sqlite3.Row]:
        return list(self.conn.execute("SELECT * FROM reports WHERE match_id = ? ORDER BY side", (match_id,)))
