"""Elo rating arithmetic (zero-sum, integer ratings) and the ranked titles the ratings hold."""

from __future__ import annotations

from typing import Any

INITIAL = 1500
K_FACTOR = 32

# The ranked titles by Elo, lowest to highest: each holds from its Elo up to the next one's, the lowest below 1300.
# A new player starts at INITIAL, a Kombatant; a set against an equal player moves 16, so Warrior is four set wins
# away and Apprentice four losses. The bands narrow above it, where a set against lower rated players gains less. Set
# from the live ladders on 2026-11-03: 111 ranked records after nine days, 1416 to 1717, nine in ten 1450 to 1599.
RANKS: tuple[tuple[int | None, str], ...] = (
    (None, "Ed Boon"),
    (1300, "Apprentice"),
    (1450, "Kombatant"),
    (1550, "Warrior"),
    (1600, "Gladiator"),
    (1650, "Master"),
    (1700, "Grand Master"),
    (1750, "Demigod"),
    (1800, "God"),
    (1850, "Elder God"),
    (1900, "Elder God Supreme"),
    (2000, "Elder God of Destruction"),
)


def title(rating: int) -> str:
    """The ranked title a rating holds."""
    for floor, name in reversed(RANKS):
        if floor is None or rating >= floor:
            return name
    raise AssertionError("RANKS starts with a title for every rating")


def ladder() -> list[dict[str, Any]]:
    """The titles in order with the Elo each starts at (None: the lowest, below the next one's)."""
    return [{"title": name, "from": floor} for floor, name in RANKS]


def expected(rating_a: float, rating_b: float) -> float:
    """Probability that A beats B."""
    return 1.0 / (1.0 + 10.0 ** ((rating_b - rating_a) / 400.0))


def delta(rating_a: int, rating_b: int, score_a: float, k: int = K_FACTOR) -> int:
    """Rating change of A (B changes by the negative), rounded half away from zero."""
    raw = k * (score_a - expected(rating_a, rating_b))
    return int(raw + 0.5) if raw >= 0 else -int(-raw + 0.5)


def update(rating_a: int, rating_b: int, score_a: float, k: int = K_FACTOR) -> tuple[int, int]:
    """New (A, B) ratings after a game where A scored `score_a` (1 win, 0 loss)."""
    d = delta(rating_a, rating_b, score_a, k)
    return rating_a + d, rating_b - d
