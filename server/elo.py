"""Elo rating arithmetic (zero-sum, integer ratings) and the ranked titles the ratings hold."""

from __future__ import annotations

from typing import Any

INITIAL = 1500
K_FACTOR = 32

# The ranked titles by Elo, lowest to highest: each holds from its Elo up to the next one's, the lowest below 1425.
# Each has its badge (art/ranks, the launcher and the games draw them by title). A new player starts at INITIAL, a
# Fighter; a set against an equal player moves 16, so two set wins make an Expert and two losses a Learner. Bands of
# 50 above, wider at the top, where a set against lower rated players gains less. Set 2026-11-07, when 16 badges
# replaced the 12 titles of 0.1.21: on 2026-11-03 the live ladders held 111 ranked records, 1416 to 1717, 95 of them
# 1450 to 1549.
RANKS: tuple[tuple[int | None, str], ...] = (
    (None, "Beginner"),
    (1425, "Learner"),
    (1475, "Fighter"),
    (1525, "Expert"),
    (1575, "Warrior"),
    (1625, "Gladiator"),
    (1675, "Master"),
    (1725, "Commander"),
    (1775, "Grandmaster"),
    (1825, "Warlord"),
    (1875, "Chosen One"),
    (1925, "Demi God"),
    (1975, "God"),
    (2025, "Elder God"),
    (2100, "Titan"),
    (2200, "Eldest God"),
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
