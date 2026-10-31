#!/usr/bin/env python3
"""Unit tests for the resumable tournament-series orchestration."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from run_tournament_series import pairing_issues, safe_series_id, summarize_tournament, validate_series_config
from local_match_lab import tournament_game_plan


def test_config_validation() -> None:
    assert validate_series_config(10, 100, "3+0", 64, 1, 4, 3.0, 600) == "3+0"
    for values in (
        (0, 100, "3+0", 64, 1, 4, 3.0, 600),
        (10, 99, "3+0", 64, 1, 4, 3.0, 600),
        (10, 100, "0+0", 64, 1, 4, 3.0, 600),
        (10, 100, "3+0", 8, 1, 4, 3.0, 600),
    ):
        try:
            validate_series_config(*values)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Expected config rejection: {values}")


def test_safe_series_id() -> None:
    assert safe_series_id("sf19-max-pilot_01") == "sf19-max-pilot_01"
    for value in ("", "../escape", "has spaces", "x" * 65):
        try:
            safe_series_id(value)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Expected unsafe series ID rejection: {value!r}")


def test_paired_color_opening_rotation() -> None:
    white1, opening1, pair1 = tournament_game_plan(1, 0)
    black1, opening2, pair2 = tournament_game_plan(1, 1)
    white2, opening3, pair3 = tournament_game_plan(1, 2)
    white_other_tournament, other_opening, _ = tournament_game_plan(2, 0)
    assert white1 is True and black1 is False and white2 is True
    assert opening1 == opening2 and opening3 != opening1
    assert pair1 == pair2 == 1 and pair3 == 2
    assert white_other_tournament is True and other_opening != opening1


def test_summary_and_integrity() -> None:
    rows = [
        {"tournament_game_number": 1, "our_white": True, "opening": "e2e4 e7e5", "our_result": "win", "termination": "checkmate", "time_control": "3+0"},
        {"tournament_game_number": 2, "our_white": False, "opening": "e2e4 e7e5", "our_result": "draw", "termination": "stalemate", "time_control": "3+0"},
        {"tournament_game_number": 3, "our_white": True, "opening": "", "our_result": "loss", "termination": "time_forfeit", "time_control": "3+0"},
        {"tournament_game_number": 4, "our_white": False, "opening": "", "our_result": "win", "termination": "checkmate", "time_control": "3+0"},
    ]
    assert pairing_issues(rows) == []
    summary = summarize_tournament(rows, 4)
    assert summary["completed_games"] == 4
    assert summary["wins"] == 2 and summary["draws"] == 1 and summary["losses"] == 1
    assert summary["score_fraction"] == 0.625
    assert summary["paired_color_opening_check"] == "PASS"
    bad = [dict(rows[0]), dict(rows[1])]
    bad[1]["opening"] = "d2d4 d7d5"
    assert pairing_issues(bad)


if __name__ == "__main__":
    test_config_validation()
    test_safe_series_id()
    test_paired_color_opening_rotation()
    test_summary_and_integrity()
    print("Tournament-series tests passed.")
