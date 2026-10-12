#!/usr/bin/env python3
"""Train NextGen's compact NNUE from Stockfish-labelled missed-move positions.

The input JSONL is produced by tools/sf19_max_train.py and contains at least
  fen, best_cp
best_cp is Stockfish's score from the side-to-move perspective.

The exporter intentionally matches nnue/nnue.cpp's fixed-point layout:
  FT weights: [feature][hidden], QA-scaled
  L1 weights: [input][hidden], QB-scaled
  L1 bias: QA*QB-scaled
  L2 weights: QB-scaled
  L2 bias: QB*QB-scaled
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

FT_SIZE = 5120
HT1 = 256
HT2 = 32
QA = 255
QB = 64
SCORE_SCALE = 400


def piece_to_type(piece: Any, perspective_color: bool) -> int:
    import chess
    if piece.piece_type == chess.KING:
        return -1
    base = 0 if piece.color == perspective_color else 5
    return base + {
        chess.PAWN: 0,
        chess.KNIGHT: 1,
        chess.BISHOP: 2,
        chess.ROOK: 3,
        chess.QUEEN: 4,
    }[piece.piece_type]


def king_bucket(square: int) -> int:
    # This compact feature map uses one bucket per king rank (8 buckets).
    return square // 8


def fen_to_features(fen: str) -> tuple[list[int], list[int]]:
    import chess
    board = chess.Board(fen)
    white_king = board.king(chess.WHITE)
    black_king = board.king(chess.BLACK)
    if white_king is None or black_king is None:
        return [], []

    white_features: list[int] = []
    black_features: list[int] = []
    for square in chess.SQUARES:
        piece = board.piece_at(square)
        if piece is None or piece.piece_type == chess.KING:
            continue

        white_type = piece_to_type(piece, chess.WHITE)
        white_feature = king_bucket(white_king) * 640 + white_type * 64 + square
        if 0 <= white_feature < FT_SIZE:
            white_features.append(white_feature)

        mirrored_square = square ^ 56
        black_type = piece_to_type(piece, chess.BLACK)
        black_feature = king_bucket(black_king ^ 56) * 640 + black_type * 64 + mirrored_square
        if 0 <= black_feature < FT_SIZE:
            black_features.append(black_feature)

    return white_features, black_features


def load_positions(path: Path, max_cp: int) -> list[dict[str, Any]]:
    import chess
    # Average repeated board positions instead of treating every duplicate FEN
    # (possibly with a different move counter) as an independent sample.
    aggregates: dict[str, dict[str, Any]] = {}
    malformed = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            record = json.loads(line)
            fen = str(record["fen"])
            score = record.get("best_cp")
            if not isinstance(score, (int, float)):
                continue
            board = chess.Board(fen)
            if board.king(chess.WHITE) is None or board.king(chess.BLACK) is None:
                continue
            white, black = fen_to_features(fen)
            # King-versus-king positions have no active NNUE features; they
            # are drawn by rule and do not teach a useful static evaluation.
            if not white or not black:
                continue
            key = " ".join(fen.split()[:4])  # board, side, castling, en-passant
            clipped = max(-max_cp, min(max_cp, int(score)))
            slot = aggregates.setdefault(
                key, {"key": key, "fen": fen, "sum_cp": 0, "count": 0,
                      "white": white, "black": black}
            )
            slot["sum_cp"] += clipped
            slot["count"] += 1
        except Exception:
            malformed += 1

    positions = []
    for row in aggregates.values():
        row["target_cp"] = row.pop("sum_cp") / row.pop("count")
        positions.append(row)
    print(f"Loaded {len(positions)} unique positions from {path} (skipped malformed={malformed})")
    if len(positions) < 100:
        raise SystemExit("Refusing to train: fewer than 100 usable teacher positions.")
    return positions


class FeatureDataset(Dataset):
    def __init__(self, rows: list[dict[str, Any]]) -> None:
        self.rows = rows

    def __len__(self) -> int:
        return len(self.rows)

    def __getitem__(self, index: int):
        row = self.rows[index]
        return (
            row["white"],
            row["black"],
            torch.tensor(float(row["target_cp"]) / SCORE_SCALE, dtype=torch.float32),
        )


def collate_sparse(batch):
    white_indices: list[int] = []
    black_indices: list[int] = []
    white_offsets: list[int] = []
    black_offsets: list[int] = []
    targets = []
    for white, black, target in batch:
        white_offsets.append(len(white_indices))
        black_offsets.append(len(black_indices))
        white_indices.extend(white)
        black_indices.extend(black)
        targets.append(target)
    return (
        torch.tensor(white_indices, dtype=torch.long),
        torch.tensor(white_offsets, dtype=torch.long),
        torch.tensor(black_indices, dtype=torch.long),
        torch.tensor(black_offsets, dtype=torch.long),
        torch.stack(targets).reshape(-1, 1),
    )


class TeacherNNUE(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.ft = nn.EmbeddingBag(FT_SIZE, HT1, mode="sum")
        self.ft_bias = nn.Parameter(torch.zeros(HT1))
        self.l1 = nn.Linear(HT1 * 2, HT2)
        self.l2 = nn.Linear(HT2, 1)

        nn.init.uniform_(self.ft.weight, -0.01, 0.01)
        nn.init.uniform_(self.ft_bias, -0.02, 0.02)
        nn.init.xavier_uniform_(self.l1.weight)
        nn.init.zeros_(self.l1.bias)
        nn.init.xavier_uniform_(self.l2.weight)
        nn.init.zeros_(self.l2.bias)

    def forward(self, white_indices, white_offsets, black_indices, black_offsets):
        white = torch.clamp(self.ft(white_indices, white_offsets) + self.ft_bias, 0.0, 1.0)
        black = torch.clamp(self.ft(black_indices, black_offsets) + self.ft_bias, 0.0, 1.0)
        merged = torch.cat((white, black), dim=1)
        hidden = torch.clamp(self.l1(merged), 0.0, 1.0)
        return self.l2(hidden)


def export_binary(model: TeacherNNUE, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # EmbeddingBag weights already use [feature][hidden] order.
    ft_w = np.rint(model.ft.weight.detach().cpu().numpy() * QA).astype(np.int16)
    ft_b = np.rint(model.ft_bias.detach().cpu().numpy() * QA).astype(np.int16)
    # C++ indexes L1 as [input][hidden], while torch Linear stores [hidden][input].
    l1_w = np.rint(model.l1.weight.detach().cpu().numpy().T * QB).astype(np.int16)
    l1_b = np.rint(model.l1.bias.detach().cpu().numpy() * QA * QB).astype(np.int32)
    l2_w = np.rint(model.l2.weight.detach().cpu().numpy().reshape(-1) * QB).astype(np.int16)
    l2_b = np.asarray(
        [round(float(model.l2.bias.detach().cpu().item()) * QB * QB)],
        dtype=np.int32,
    )

    with output_path.open("wb") as stream:
        for array in (ft_w, ft_b, l1_w, l1_b, l2_w, l2_b):
            stream.write(array.tobytes(order="C"))
    expected = (
        FT_SIZE * HT1 * 2
        + HT1 * 2
        + HT1 * 2 * HT2 * 2
        + HT2 * 4
        + HT2 * 2
        + 4
    )
    actual = output_path.stat().st_size
    if actual != expected:
        raise RuntimeError(f"NNUE export size mismatch: expected {expected}, got {actual}")
    print(f"Exported compatible NNUE: {output_path} ({actual} bytes)")


def train(args) -> None:
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    torch.set_num_threads(max(1, args.threads))
    device = torch.device("cpu") if args.cpu else torch.device(
        "cuda" if torch.cuda.is_available() else "cpu"
    )

    positions = load_positions(Path(args.dataset), args.max_cp)
    # Stable FEN-based split ensures duplicate positions never leak into both
    # train and validation data.
    train_rows, val_rows = [], []
    for row in positions:
        bucket = int(hashlib.sha256(row["key"].encode("utf-8")).hexdigest()[:8], 16) % 10
        (val_rows if bucket == 0 else train_rows).append(row)
    if not val_rows:
        val_rows.append(train_rows.pop())
    print(f"Device={device}; train={len(train_rows)}; validation={len(val_rows)}; compact FT={FT_SIZE}")

    train_loader = DataLoader(
        FeatureDataset(train_rows), batch_size=args.batch, shuffle=True,
        num_workers=0, collate_fn=collate_sparse,
    )
    val_loader = DataLoader(
        FeatureDataset(val_rows), batch_size=args.batch, shuffle=False,
        num_workers=0, collate_fn=collate_sparse,
    )
    model = TeacherNNUE().to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    loss_fn = nn.SmoothL1Loss(beta=0.25)
    best_val = float("inf")
    best_epoch = -1
    best_state = None
    stale_epochs = 0

    for epoch in range(1, args.epochs + 1):
        model.train()
        train_loss = 0.0
        batches = 0
        for wi, wo, bi, bo, target in train_loader:
            wi, wo, bi, bo, target = (
                wi.to(device), wo.to(device), bi.to(device), bo.to(device), target.to(device)
            )
            optimizer.zero_grad(set_to_none=True)
            prediction = model(wi, wo, bi, bo)
            loss = loss_fn(prediction, target)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            train_loss += float(loss.item())
            batches += 1

        model.eval()
        absolute_error = 0.0
        squared_error = 0.0
        val_count = 0
        with torch.no_grad():
            for wi, wo, bi, bo, target in val_loader:
                wi, wo, bi, bo, target = (
                    wi.to(device), wo.to(device), bi.to(device), bo.to(device), target.to(device)
                )
                pred = model(wi, wo, bi, bo)
                delta_cp = (pred - target) * SCORE_SCALE
                absolute_error += float(delta_cp.abs().sum().item())
                squared_error += float((delta_cp * delta_cp).sum().item())
                val_count += target.numel()

        val_mae = absolute_error / max(1, val_count)
        val_rmse = (squared_error / max(1, val_count)) ** 0.5
        print(
            f"epoch={epoch:02d} train_loss={train_loss / max(1, batches):.5f} "
            f"val_mae_cp={val_mae:.1f} val_rmse_cp={val_rmse:.1f}"
        )
        if val_mae < best_val:
            best_val = val_mae
            best_epoch = epoch
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
            stale_epochs = 0
        else:
            stale_epochs += 1
        if stale_epochs >= args.patience:
            print(f"Early stop after {epoch} epochs; best epoch {best_epoch}")
            break

    if best_state is None:
        raise RuntimeError("No valid NNUE checkpoint was produced.")
    model.load_state_dict(best_state)
    model.to("cpu")
    export_binary(model, Path(args.out).with_suffix(".nnue"))

    report = {
        "dataset": str(Path(args.dataset)),
        "unique_positions": len(positions),
        "train_positions": len(train_rows),
        "validation_positions": len(val_rows),
        "max_cp_clamp": args.max_cp,
        "best_epoch": best_epoch,
        "validation_mae_cp": round(best_val, 2),
        "network_path": str(Path(args.out).with_suffix(".nnue")),
        "feature_count": FT_SIZE,
        "hidden_1": HT1,
        "hidden_2": HT2,
    }
    report_path = Path(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", default="datasets/missed_best_moves.jsonl")
    parser.add_argument("--out", default="/tmp/nextgen_nnue.pt")
    parser.add_argument("--report", default="networks/nnue_training_report.json")
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--patience", type=int, default=4)
    parser.add_argument("--lr", type=float, default=0.001)
    parser.add_argument("--weight-decay", type=float, default=0.0001)
    parser.add_argument("--max-cp", type=int, default=1200)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--seed", type=int, default=20261012)
    parser.add_argument("--cpu", action="store_true")
    args = parser.parse_args()
    train(args)


if __name__ == "__main__":
    main()
