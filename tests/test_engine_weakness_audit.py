#!/usr/bin/env python3
"""Unit checks for PGN extraction and mistake severity without launching Stockfish."""
from __future__ import annotations

import io
import sys
import tempfile
from pathlib import Path

import chess
import chess.pgn

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from analyze_engine_weakness import collect_nextgen_positions, severity


def test_extract_nextgen_black_moves() -> None:
    game = chess.pgn.Game()
    game.headers["White"] = "Stockfish 19 MAX"
    game.headers["Black"] = "NextGen"
    game.headers["Result"] = "1/2-1/2"
    board = game.board()
    node = game
    for san in ("e4", "e5", "Nf3", "Nc6", "Bb5", "a6"):
        move = board.parse_san(san)
        node = node.add_variation(move)
        board.push(move)

    with tempfile.TemporaryDirectory(prefix="nextgen-audit-test-") as folder:
        path = Path(folder) / "sample.pgn"
        path.write_text(str(game) + "\n\n", encoding="utf-8")
        rows = collect_nextgen_positions(path)

    assert len(rows) == 3, rows
    assert all(row["color"] == "black" for row in rows), rows
    assert [row["played_san"] for row in rows] == ["e5", "Nc6", "a6"]
    assert all(row["fen"].split()[1] == "b" for row in rows)


def test_severity_boundaries() -> None:
    assert severity(79) == "below_threshold"
    assert severity(80) == "inaccuracy"
    assert severity(150) == "mistake"
    assert severity(300) == "blunder"
    assert severity(800) == "catastrophic"


if __name__ == "__main__":
    test_extract_nextgen_black_moves()
    test_severity_boundaries()
    print("Engine weakness audit tests passed.")
