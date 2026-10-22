#!/usr/bin/env python3
"""Fast regression test for the Python NNUE exporter and fixed-point layout."""
from __future__ import annotations

import tempfile
import sys
import json
from pathlib import Path

import numpy as np
import torch

# Make the repository root importable when this file is run directly by CI.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from training.train_nnue_teacher import (
    FT_SIZE, HT1, HT2, QA, QB, NNUE_MAGIC, NNUE_FORMAT_VERSION,
    TeacherNNUE, export_binary, fen_to_features, load_positions,
)


def test_side_to_move_target_and_feature_order() -> None:
    fen_white = "7k/P7/8/8/8/8/8/7K w - - 0 1"
    fen_black = "7k/P7/8/8/8/8/8/7K b - - 0 1"
    with tempfile.TemporaryDirectory(prefix="nextgen-nnue-pov-") as folder:
        path = Path(folder) / "teacher_eval_positions.jsonl"
        rows = [
            {"fen": fen_white, "best_cp": 400, "teacher_depth": 20},
            {"fen": fen_black, "best_cp": -250, "teacher_depth": 20},
        ]
        path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
        loaded = load_positions([path], max_cp=1200)
        by_turn = {row["fen"].split()[1]: row for row in loaded}
        white_features, black_features = fen_to_features(fen_white)
        assert abs(by_turn["w"]["target_cp"] - 400) < 1e-6
        assert abs(by_turn["b"]["target_cp"] + 250) < 1e-6
        assert by_turn["w"]["us"] == white_features
        assert by_turn["w"]["them"] == black_features
        assert by_turn["b"]["us"] == black_features
        assert by_turn["b"]["them"] == white_features


def main() -> None:
    test_side_to_move_target_and_feature_order()
    torch.manual_seed(1)
    model = TeacherNNUE().cpu().eval()
    with torch.no_grad():
        model.ft.weight.zero_()
        model.ft_bias.zero_()
        model.l1.weight.zero_()
        model.l1.bias.zero_()
        model.l2.weight.zero_()
        model.l2.bias.zero_()

        # One active feature produces exactly +1.0 network output:
        # feature 48 -> accumulator lane 0 -> hidden lane 0 -> output.
        model.ft.weight[48, 0] = 1.0
        model.l1.weight[0, 0] = 1.0
        model.l2.weight[0, 0] = 1.0

        indices = torch.tensor([48], dtype=torch.long)
        offsets = torch.tensor([0], dtype=torch.long)
        empty_indices = torch.tensor([], dtype=torch.long)
        prediction = model(indices, offsets, empty_indices, offsets)
        assert abs(float(prediction.item()) - 1.0) < 1e-6, prediction.item()

    with tempfile.TemporaryDirectory(prefix="nextgen-nnue-export-") as folder:
        output = Path(folder) / "synthetic.nnue"
        export_binary(model, output)
        expected_size = (
            8 + 4 + FT_SIZE * HT1 * 2 + HT1 * 2
            + HT1 * 2 * HT2 * 2 + HT2 * 4 + HT2 * 2 + 4
        )
        assert output.stat().st_size == expected_size

        raw = output.read_bytes()
        assert raw[:8] == NNUE_MAGIC
        assert int.from_bytes(raw[8:12], "little", signed=False) == NNUE_FORMAT_VERSION
        offset = 12

        def read_i16(count: int) -> np.ndarray:
            nonlocal offset
            size = count * 2
            part = np.frombuffer(raw[offset:offset + size], dtype=np.int16)
            offset += size
            return part

        def read_i32(count: int) -> np.ndarray:
            nonlocal offset
            size = count * 4
            part = np.frombuffer(raw[offset:offset + size], dtype=np.int32)
            offset += size
            return part

        ft_weights = read_i16(FT_SIZE * HT1).reshape(FT_SIZE, HT1)
        ft_bias = read_i16(HT1)
        l1_weights = read_i16(2 * HT1 * HT2).reshape(2 * HT1, HT2)
        l1_bias = read_i32(HT2)
        l2_weights = read_i16(HT2)
        l2_bias = read_i32(1)

        assert int(ft_weights[48, 0]) == QA
        assert int(ft_bias[0]) == 0
        assert int(l1_weights[0, 0]) == QB
        assert int(l1_bias[0]) == 0
        assert int(l2_weights[0]) == QB
        assert int(l2_bias[0]) == 0
        assert offset == expected_size
        print(f"NNUE v{NNUE_FORMAT_VERSION} exporter regression passed: {expected_size} bytes; synthetic output +400 cp")


if __name__ == "__main__":
    main()
