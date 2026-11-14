"""SQLite storage: accounts, login tokens, per game/mode stats, match log, result reports, ranked sets, and the
Respect Points King of the Hill spectators give (each rating kept, and each player's sum per game and mode)."""

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
CREATE TABLE IF NOT EXISTS sets (
    id            TEXT PRIMARY KEY,
    game          TEXT NOT NULL,
    mode          TEXT NOT NULL,
    p1            INTEGER NOT NULL,
    p2            INTEGER NOT NULL,
    created       REAL NOT NULL,
    ended         REAL NOT NULL,
    outcome       TEXT NOT NULL,
    winner        INTEGER,
    p1_wins       INTEGER NOT NULL,
    p2_wins       INTEGER NOT NULL,
    p1_elo_before INTEGER,
    p1_elo_after  INTEGER,
    p2_elo_before INTEGER,
    p2_elo_after  INTEGER
);
CREATE TABLE IF NOT EXISTS respect (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    game       TEXT NOT NULL,
    mode       TEXT NOT NULL,
    points     INTEGER NOT NULL DEFAULT 0,
    ratings    INTEGER NOT NULL DEFAULT 0,
    updated    REAL,
    PRIMARY KEY (account_id, game, mode)
);
CREATE INDEX IF NOT EXISTS respect_board ON respect (game, mode, points DESC);
CREATE TABLE IF NOT EXISTS ratings (
    match_id TEXT NOT NULL,
    rater    INTEGER NOT NULL,
    winner   INTEGER NOT NULL,
    game     TEXT NOT NULL,
    mode     TEXT NOT NULL,
    score    INTEGER NOT NULL CHECK (score BETWEEN 1 AND 10),
    rated    REAL NOT NULL,
    PRIMARY KEY (match_id, rater)
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

    def backup(self, path: str) -> None:
        """A complete copy of the database in a new file at `path`."""
        copy = sqlite3.connect(path)
        try:
            self.conn.backup(copy)
        finally:
            copy.close()

    def reset_ranked(self) -> int:
        """Everyone back to the start of the ranked ladder, in every game and mode: the ranked records go (Elo, wins
        and losses), so each player is at the starting Elo and off the leaderboard until their next ranked set. Kasual
        records, accounts and the match history stay. Returns how many records went."""
        with self.transaction() as conn:
            return conn.execute("DELETE FROM stats WHERE kind = 'ranked'").rowcount

    def restore_ranked_records(self, copy_path: str) -> tuple[int, int]:
        """Every game's ranked records from a copy of this database made before a rank reset. Each record's Elo goes back
        to what it was in the copy, and its wins and losses are added to what has been played since; one with nothing
        played since comes back as it was, and one the copy does not have stays as it is. Returns (records combined with
        what was played since, records brought back)."""
        same = "o.account_id = stats.account_id AND o.game = stats.game AND o.mode = stats.mode AND o.kind = 'ranked'"
        self.conn.execute("ATTACH DATABASE ? AS old", (copy_path,))
        try:
            with self.transaction() as conn:
                added = conn.execute(
                    f"UPDATE stats SET elo = (SELECT o.elo FROM old.stats AS o WHERE {same}), "
                    f"wins = wins + (SELECT o.wins FROM old.stats AS o WHERE {same}), "
                    f"losses = losses + (SELECT o.losses FROM old.stats AS o WHERE {same}) "
                    f"WHERE kind = 'ranked' AND EXISTS (SELECT 1 FROM old.stats AS o WHERE {same})").rowcount
                back = conn.execute(
                    "INSERT INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
                    "SELECT o.account_id, o.game, o.mode, 'ranked', o.elo, o.wins, o.losses, o.updated FROM old.stats AS o "
                    "JOIN accounts AS a ON a.id = o.account_id WHERE o.kind = 'ranked' AND NOT EXISTS (SELECT 1 FROM stats AS s "
                    "WHERE s.account_id = o.account_id AND s.game = o.game AND s.mode = o.mode AND s.kind = 'ranked')").rowcount
        finally:
            self.conn.execute("DETACH DATABASE old")
        return added, back

    def recount_ranked_records(self) -> int:
        """Every ranked record's wins and losses recounted as the sets won and lost in the log of ranked sets (every set
        since 0.1.5; one without a winner counts for neither), for records that counted matches before 1.0.1. Elo and the
        Kasual records stay. Returns how many records changed."""
        won = ("SELECT COUNT(*) FROM sets AS t WHERE t.game = stats.game AND t.mode = stats.mode "
               "AND t.winner = stats.account_id")
        lost = ("SELECT COUNT(*) FROM sets AS t WHERE t.game = stats.game AND t.mode = stats.mode "
                "AND t.winner IS NOT NULL AND t.winner != stats.account_id AND stats.account_id IN (t.p1, t.p2)")
        with self.transaction() as conn:
            return conn.execute(f"UPDATE stats SET wins = ({won}), losses = ({lost}) "
                                f"WHERE kind = 'ranked' AND (wins != ({won}) OR losses != ({lost}))").rowcount

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
                entry = {"elo": row["elo"], "title": elo.title(row["elo"]), **entry}
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

    def record_ranked_set(self, winner_id: int, loser_id: int, game: str, mode: str,
                          now: float | None = None) -> dict[int, tuple[int, int]]:
        """Apply one ranked set won (or forfeited by the loser): a set won and a set lost on the two ranked records
        (they count sets, not the matches in them) and both Elo changes; returns {account_id: (elo_before,
        elo_after)}."""
        now = now or time.time()
        with self.transaction() as conn:
            before_w = self.rating(winner_id, game, mode)["elo"]
            before_l = self.rating(loser_id, game, mode)["elo"]
            after_w, after_l = elo.set_result(before_w, before_l)  # the winner gains twice what the loser loses
            self._bump(conn, winner_id, game, mode, "ranked", True, after_w, now)
            self._bump(conn, loser_id, game, mode, "ranked", False, after_l, now)
        return {winner_id: (before_w, after_w), loser_id: (before_l, after_l)}

    def log_set(self, set_id: str, game: str, mode: str, p1: int, p2: int, created: float, ended: float,
                outcome: str, winner: int | None, p1_wins: int, p2_wins: int,
                changes: dict[int, tuple[int, int]] | None) -> None:
        def elo_of(account_id: int, i: int) -> int | None:
            return changes[account_id][i] if changes and account_id in changes else None
        self.conn.execute(
            "INSERT OR REPLACE INTO sets (id, game, mode, p1, p2, created, ended, outcome, winner, p1_wins, p2_wins, "
            "p1_elo_before, p1_elo_after, p2_elo_before, p2_elo_after) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, ?, ?)",
            (set_id, game, mode, p1, p2, created, ended, outcome, winner, p1_wins, p2_wins,
             elo_of(p1, 0), elo_of(p1, 1), elo_of(p2, 0), elo_of(p2, 1)),
        )

    def set_row(self, set_id: str) -> sqlite3.Row | None:
        return self.conn.execute("SELECT * FROM sets WHERE id = ?", (set_id,)).fetchone()

    def leaderboard(self, game: str, mode: str, limit: int, offset: int) -> tuple[list[dict[str, Any]], int]:
        """The ranked players of a game and mode by Elo, each with their Respect Points there too."""
        total = self.conn.execute(
            "SELECT COUNT(*) FROM stats WHERE game = ? AND mode = ? AND kind = 'ranked' AND wins + losses > 0",
            (game, mode),
        ).fetchone()[0]
        rows = []
        for i, row in enumerate(
            self.conn.execute(
                "SELECT a.name, s.elo, s.wins, s.losses, COALESCE(r.points, 0) AS respect FROM stats s "
                "JOIN accounts a ON a.id = s.account_id "
                "LEFT JOIN respect r ON r.account_id = s.account_id AND r.game = s.game AND r.mode = s.mode "
                "WHERE s.game = ? AND s.mode = ? AND s.kind = 'ranked' AND s.wins + s.losses > 0 "
                "ORDER BY s.elo DESC, s.wins DESC, a.name COLLATE NOCASE ASC LIMIT ? OFFSET ?",
                (game, mode, limit, offset),
            )
        ):
            rows.append({"rank": offset + i + 1, "name": row["name"], "elo": row["elo"], "title": elo.title(row["elo"]),
                         "wins": row["wins"], "losses": row["losses"], "respect": row["respect"]})
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
        return {"rank": int(better) + 1, "elo": row["elo"], "title": elo.title(row["elo"]), "wins": row["wins"],
                "losses": row["losses"], "respect": self.respect(account_id, game, mode)["points"]}

    # --- Respect Points (King of the Hill) ------------------------------------------------------

    def add_rating(self, match_id: str, rater: int, winner: int, game: str, mode: str, score: int,
                   now: float | None = None) -> bool:
        """A spectator's rating of a King of the Hill match's winner (1 to 10): kept, and added to the winner's Respect
        Points in that game and mode. False when that spectator had rated that match already (nothing changes)."""
        now = now or time.time()
        with self.transaction() as conn:
            added = conn.execute(
                "INSERT OR IGNORE INTO ratings (match_id, rater, winner, game, mode, score, rated) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)", (match_id, rater, winner, game, mode, score, now)).rowcount
            if added:
                conn.execute("INSERT OR IGNORE INTO respect (account_id, game, mode, points, ratings, updated) "
                             "VALUES (?, ?, ?, 0, 0, ?)", (winner, game, mode, now))
                conn.execute("UPDATE respect SET points = points + ?, ratings = ratings + 1, updated = ? "
                             "WHERE account_id = ? AND game = ? AND mode = ?", (score, now, winner, game, mode))
        return bool(added)

    def respect(self, account_id: int, game: str, mode: str) -> dict[str, int]:
        """A player's Respect Points in a game and mode, and how many ratings gave them."""
        row = self.conn.execute("SELECT points, ratings FROM respect WHERE account_id = ? AND game = ? AND mode = ?",
                                (account_id, game, mode)).fetchone()
        return {"points": row["points"], "ratings": row["ratings"]} if row else {"points": 0, "ratings": 0}

    def respect_all(self, account_id: int) -> dict[tuple[str, str], dict[str, int]]:
        """Every game and mode a player has Respect Points in."""
        return {(row["game"], row["mode"]): {"points": row["points"], "ratings": row["ratings"]}
                for row in self.conn.execute("SELECT game, mode, points, ratings FROM respect WHERE account_id = ? "
                                             "ORDER BY game, mode", (account_id,))}

    def ratings(self, match_id: str) -> list[sqlite3.Row]:
        return list(self.conn.execute("SELECT * FROM ratings WHERE match_id = ? ORDER BY rated", (match_id,)))

    _RESPECT_ROWS = (
        "SELECT a.name, r.points, r.ratings, s.elo, s.wins, s.losses FROM respect r "
        "JOIN accounts a ON a.id = r.account_id "
        "LEFT JOIN stats s ON s.account_id = r.account_id AND s.game = r.game AND s.mode = r.mode AND s.kind = 'ranked' "
        "WHERE r.game = ? AND r.mode = ? AND r.points > 0 ")

    def _respect_row(self, row: sqlite3.Row) -> dict[str, Any]:
        # their ranked record there, or the start of one (as the lobbies show a player new to ranked)
        rating = row["elo"] if row["elo"] is not None else elo.INITIAL
        return {"name": row["name"], "respect": row["points"], "ratings": row["ratings"], "elo": rating,
                "title": elo.title(rating), "wins": row["wins"] or 0, "losses": row["losses"] or 0}

    def respect_board(self, game: str, mode: str, limit: int, offset: int) -> tuple[list[dict[str, Any]], int]:
        """Everyone with Respect Points in a game and mode, the most first (then the fewer ratings that gave them,
        then by name), each with their ranked rating there."""
        total = self.conn.execute("SELECT COUNT(*) FROM respect WHERE game = ? AND mode = ? AND points > 0",
                                  (game, mode)).fetchone()[0]
        rows = [{"rank": offset + i + 1, **self._respect_row(row)} for i, row in enumerate(self.conn.execute(
            self._RESPECT_ROWS + "ORDER BY r.points DESC, r.ratings ASC, a.name COLLATE NOCASE ASC LIMIT ? OFFSET ?",
            (game, mode, limit, offset)))]
        return rows, int(total)

    def respect_rank_of(self, account_id: int, game: str, mode: str) -> dict[str, Any] | None:
        row = self.conn.execute(self._RESPECT_ROWS + "AND r.account_id = ?", (game, mode, account_id)).fetchone()
        if row is None:
            return None
        better = self.conn.execute(
            "SELECT COUNT(*) FROM respect r JOIN accounts a ON a.id = r.account_id "
            "WHERE r.game = ? AND r.mode = ? AND r.points > 0 AND "
            "(r.points > ? OR (r.points = ? AND r.ratings < ?) OR "
            " (r.points = ? AND r.ratings = ? AND a.name < ? COLLATE NOCASE))",
            (game, mode, row["points"], row["points"], row["ratings"], row["points"], row["ratings"], row["name"]),
        ).fetchone()[0]
        return {"rank": int(better) + 1, **self._respect_row(row)}

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
