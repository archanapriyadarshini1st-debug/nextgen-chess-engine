#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from quality_clock_match import load_openings, opening_for_pair, validate_opening_moves, verify_pairs


def main() -> None:
    moves = "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6"
    fen = validate_opening_moves(moves)
    assert " w " in fen
    try:
        validate_opening_moves("e2e4 e7e5 g1f3")
        raise AssertionError("odd-ply opening should be rejected")
    except ValueError:
        pass
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "openings.jsonl"
        rows = [
            {"moves": moves},
            {"moves": "d2d4 d7d5 c2c4 e7e6 g1f3 g8f6 b1c3 f8e7"},
        ]
        path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
        openings = load_openings(path)
        assert len(openings) == 2
        assert opening_for_pair(openings, 1, 0) == openings[1]
    good_pair = [
        {"paired_opening_id": 7, "our_white": True, "opening": moves},
        {"paired_opening_id": 7, "our_white": False, "opening": moves},
    ]
    assert verify_pairs(good_pair) == []
    assert verify_pairs(good_pair[:1])
    print("Quality opening-suite and color-pair tests passed")


if __name__ == "__main__":
    main()
