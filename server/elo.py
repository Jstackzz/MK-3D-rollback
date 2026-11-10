"""Elo rating arithmetic (zero-sum, integer ratings) and the ranked titles the ratings hold."""

from __future__ import annotations

from typing import Any

INITIAL = 475
K_FACTOR = 32

# The ranked titles by Elo, lowest to highest: each holds from its Elo up to the next one's, the lowest below 575.
# Each has its badge (art/ranks, the launcher and the games draw them by title). A new player starts at INITIAL, a
# Beginner, one step below Learner. The steps up (2026-11-10): 100 Elo a rank up to God, 120 a rank from God on.
RANKS: tuple[tuple[int | None, str], ...] = (
    (None, "Beginner"),
    (575, "Learner"),
    (675, "Fighter"),
    (775, "Expert"),
    (875, "Warrior"),
    (975, "Gladiator"),
    (1075, "Master"),
    (1175, "Commander"),
    (1275, "Grandmaster"),
    (1375, "Warlord"),
    (1475, "Chosen One"),
    (1575, "Demi God"),
    (1675, "God"),
    (1795, "Elder God"),
    (1915, "Titan"),
    (2035, "Eldest God"),
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
