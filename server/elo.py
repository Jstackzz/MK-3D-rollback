"""Elo rating arithmetic (zero-sum, integer ratings) and the ranked titles the ratings hold."""

from __future__ import annotations

from typing import Any

INITIAL = 475
K_FACTOR = 32

# The ranked titles by Elo, lowest to highest: each holds from its Elo up to the next one's, the lowest below 975.
# Each has its badge (art/ranks, the launcher and the games draw them by title). A new player starts at INITIAL, a
# Beginner, one step (500) below Learner; a record made when the start was 1500 (a Fighter) keeps its Elo and its title.
# The steps up are set by rank (2026-11-09): 500 up to Expert, 750 from Expert, 1000 each from Warrior, 1500 each from
# Commander, 2000 each from Warlord, 3000 each from Demi God, 3500 each from Elder God.
RANKS: tuple[tuple[int | None, str], ...] = (
    (None, "Beginner"),
    (975, "Learner"),
    (1475, "Fighter"),
    (1975, "Expert"),
    (2725, "Warrior"),
    (3725, "Gladiator"),
    (4725, "Master"),
    (5725, "Commander"),
    (7225, "Grandmaster"),
    (8725, "Warlord"),
    (10725, "Chosen One"),
    (12725, "Demi God"),
    (15725, "God"),
    (18725, "Elder God"),
    (22225, "Titan"),
    (25725, "Eldest God"),
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
