#!/usr/bin/env python3
"""Extract legal positions from games exported by NextGen Chess Memory.

Usage:
  py -m pip install python-chess
  py scripts/build_extension_memory_dataset.py "C:/path/to/NextGen-Chess-Memory"

The script reads games/*.pgn, deduplicates positions and writes
positions/positions.jsonl. It does not label positions or train a network.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import chess
import chess.pgn


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("memory_dir", type=Path, help="Exported NextGen-Chess-Memory folder")
    args = parser.parse_args()

    games_dir = args.memory_dir / "games"
    positions_dir = args.memory_dir / "positions"
    if not games_dir.is_dir():
        raise SystemExit("Could not find games/ inside the selected memory folder.")

    positions_dir.mkdir(parents=True, exist_ok=True)
    rows: dict[str, dict] = {}
    games_read = 0
    malformed = 0
    for pgn_path in sorted(games_dir.glob("*.pgn")):
        if pgn_path.name.lower() == "all_games.pgn":
            continue
        with pgn_path.open("r", encoding="utf-8", errors="replace") as stream:
            while True:
                try:
                    game = chess.pgn.read_game(stream)
                except Exception:
                    malformed += 1
                    continue
                if game is None:
                    break
                games_read += 1
                board = game.board()
                headers = game.headers
                for ply, move in enumerate(game.mainline_moves(), start=1):
                    try:
                        fen = board.fen()
                        san = board.san(move)
                        key = " ".join(fen.split()[:4])
                        row = {
                            "fen": fen,
                            "uci_move": move.uci(),
                            "san_move": san,
                            "ply": ply,
                            "result": headers.get("Result", "*"),
                            "white": headers.get("White", "?"),
                            "black": headers.get("Black", "?"),
                            "source_game": pgn_path.name,
                        }
                        rows.setdefault(key, row)
                        board.push(move)
                    except (ValueError, AssertionError):
                        malformed += 1
                        break
                if board.is_valid():
                    fen = board.fen()
                    key = " ".join(fen.split()[:4])
                    rows.setdefault(key, {
                        "fen": fen,
                        "uci_move": None,
                        "san_move": None,
                        "ply": board.fullmove_number * 2 - (0 if board.turn else 1),
                        "result": headers.get("Result", "*"),
                        "white": headers.get("White", "?"),
                        "black": headers.get("Black", "?"),
                        "source_game": pgn_path.name,
                        "final_position": True,
                    })

    output = positions_dir / "positions.jsonl"
    with output.open("w", encoding="utf-8") as stream:
        for row in rows.values():
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")

    summary = {
        "games_read": games_read,
        "unique_positions": len(rows),
        "malformed_or_skipped": malformed,
        "output": str(output),
        "note": "These are game positions and played moves, not Stockfish evaluation labels."
    }
    (positions_dir / "build_summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    if games_read == 0 or not rows:
        raise SystemExit("No valid game positions were extracted.")


if __name__ == "__main__":
    main()
