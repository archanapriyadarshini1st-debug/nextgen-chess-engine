#!/usr/bin/env python3
"""
NNUE trainer - HalfKP-lite 5120 x 256 x 32 x 1.

What changed vs the old trainer
-------------------------------
* nn.EmbeddingBag(mode="sum") over ACTIVE FEATURE INDICES instead of a dense
  torch.zeros(41024) with ~32 ones. The old path cost 41024*4 bytes per sample
  per side -> ~84 MB for a batch of 512, plus a 41024x256 dense matmul that was
  99.9% multiplications by zero. EmbeddingBag gathers only the ~30 active rows.
* Accumulators are ordered (side-to-move, other-side) and the label is the
  side-to-move relative eval, matching nnue.cpp exactly. The old code fed
  (white, black) but trained on stm-relative labels while C++ flipped the sign
  for black, so the net was fitted against a sign-flipped target half the time.
* Feature space is 8*10*64 = 5120, kings excluded. The old 41024 only ever
  addressed 5120 slots and let kings overflow into the next bucket.
* Loss mixes the teacher eval with the actual game result (WDL), in win
  probability space, which is what stops the loss-0.0 overfit.
* Exports a versioned binary header so the engine refuses a mismatched net
  instead of silently evaluating garbage.
"""

import argparse
import json
import math
import os
import struct
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Dataset

KING_BUCKETS = 8
PIECE_TYPES = 10
FT_SIZE = KING_BUCKETS * PIECE_TYPES * 64      # 5120
THREAT_INPUTS = 2
HT1 = 256
HT2 = 32
QA = 255
QB = 64
NNUE_SCALE = 400.0
MAGIC = b"NGCEv3\0\0"


# --------------------------------------------------------------------------
# Feature extraction - MUST stay in lockstep with NNUE::make_feature in C++.
# --------------------------------------------------------------------------
def fen_to_features(fen):
    """Return (us_indices, them_indices) for the side to move."""
    import chess

    board = chess.Board(fen)
    wk, bk = board.king(chess.WHITE), board.king(chess.BLACK)
    if wk is None or bk is None:
        return None

    white, black = [], []
    for sq, piece in board.piece_map().items():
        if piece.piece_type == chess.KING:
            continue
        t = piece.piece_type - 1  # 0..4 for P N B R Q

        # white perspective: raw squares
        tw = (0 if piece.color == chess.WHITE else 5) + t
        white.append((wk // 8) * 640 + tw * 64 + sq)

        # black perspective: vertically mirrored squares
        tb = (0 if piece.color == chess.BLACK else 5) + t
        black.append(((bk ^ 56) // 8) * 640 + tb * 64 + (sq ^ 56))

    us, them = (white, black) if board.turn == chess.WHITE else (black, white)
    us_color = board.turn
    them_color = not us_color
    enemy_threats = 0
    own_threats = 0
    for sq, piece in board.piece_map().items():
        if piece.piece_type == chess.KING:
            continue
        if piece.color == them_color and board.attackers(us_color, sq):
            enemy_threats += 1
        if piece.color == us_color and board.attackers(them_color, sq):
            own_threats += 1
    threats = [min(255, enemy_threats * 255 // 8), min(255, own_threats * 255 // 8)]
    return us, them, threats


def result_to_wdl(result, stm_is_white):
    """'1-0'/'0-1'/'1/2-1/2' -> win probability for the side to move."""
    if result == "1-0":
        w = 1.0
    elif result == "0-1":
        w = 0.0
    elif result in ("1/2-1/2", "1/2"):
        w = 0.5
    else:
        return None
    return w if stm_is_white else 1.0 - w


class GameDataset(Dataset):
    """Stores active feature indices, not dense vectors."""

    def __init__(self, paths, limit=None):
        self.samples = []
        skipped = 0
        for path in paths:
            with open(path) as f:
                for line in f:
                    if limit and len(self.samples) >= limit:
                        break
                    try:
                        e = json.loads(line)
                    except json.JSONDecodeError:
                        skipped += 1
                        continue
                    fen = e.get("fen")
                    cp = e.get("eval_cp", e.get("sf_eval_cp"))
                    if fen is None or cp is None:
                        skipped += 1
                        continue
                    feats = fen_to_features(fen)
                    if feats is None or not feats[0] and not feats[1]:
                        skipped += 1
                        continue
                    us, them, threats = feats
                    stm_white = fen.split()[1] == "w"
                    wdl = result_to_wdl(e.get("result", "*"), stm_white)
                    self.samples.append(
                        (
                            np.asarray(us, dtype=np.int64),
                            np.asarray(them, dtype=np.int64),
                            np.asarray(threats, dtype=np.float32),
                            float(np.clip(cp, -3000, 3000)),
                            -1.0 if wdl is None else wdl,
                        )
                    )
        print(f"loaded {len(self.samples)} positions ({skipped} skipped)")

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, i):
        return self.samples[i]


def collate(batch):
    """Pack variable length index lists into EmbeddingBag (input, offsets)."""
    us_idx, us_off, them_idx, them_off = [], [], [], []
    threats, cps, wdls = [], [], []
    u = t = 0
    for us, them, thr, cp, wdl in batch:
        us_off.append(u)
        them_off.append(t)
        us_idx.append(us)
        them_idx.append(them)
        u += len(us)
        t += len(them)
        threats.append(thr)
        cps.append(cp)
        wdls.append(wdl)
    cat = lambda xs: torch.from_numpy(np.concatenate(xs)) if xs else torch.zeros(0, dtype=torch.long)
    return (
        cat(us_idx),
        torch.tensor(us_off, dtype=torch.long),
        cat(them_idx),
        torch.tensor(them_off, dtype=torch.long),
        torch.tensor(np.asarray(threats), dtype=torch.float32),
        torch.tensor(cps, dtype=torch.float32).unsqueeze(1),
        torch.tensor(wdls, dtype=torch.float32).unsqueeze(1),
    )


class NNUE(nn.Module):
    def __init__(self):
        super().__init__()
        self.ft = nn.EmbeddingBag(FT_SIZE, HT1, mode="sum", sparse=False)
        self.ft_bias = nn.Parameter(torch.zeros(HT1))
        self.l1 = nn.Linear(HT1 * 2 + THREAT_INPUTS, HT2)
        self.l2 = nn.Linear(HT2, 1)
        nn.init.normal_(self.ft.weight, std=0.01)

    def forward(self, us_idx, us_off, them_idx, them_off, threats):
        us = torch.clamp(self.ft(us_idx, us_off) + self.ft_bias, 0.0, 1.0)
        them = torch.clamp(self.ft(them_idx, them_off) + self.ft_bias, 0.0, 1.0)
        x = torch.cat([us, them, threats / 255.0], dim=1)  # stm FIRST + threats
        x = torch.clamp(self.l1(x), 0.0, 1.0)
        return self.l2(x)                          # stm-relative, in eval/400 units


def win_prob(cp_over_400):
    return torch.sigmoid(cp_over_400 * 400.0 / 400.0)


def train(args):
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    ds = GameDataset(args.dataset, args.limit)
    if len(ds) == 0:
        print("no data")
        return

    n_val = max(1, int(len(ds) * 0.02))
    train_ds, val_ds = torch.utils.data.random_split(
        ds, [len(ds) - n_val, n_val], generator=torch.Generator().manual_seed(0)
    )
    dl = DataLoader(train_ds, batch_size=args.batch, shuffle=True,
                    collate_fn=collate, num_workers=args.workers, drop_last=True)
    vdl = DataLoader(val_ds, batch_size=args.batch, shuffle=False, collate_fn=collate)

    model = NNUE().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-5)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=max(1, args.epochs))

    print(f"device={device} ft={FT_SIZE} ht1={HT1} ht2={HT2} "
          f"train={len(train_ds)} val={len(val_ds)} lambda={args.lam}")

    best = float("inf")
    for epoch in range(args.epochs):
        model.train()
        total = count = 0
        for us_i, us_o, th_i, th_o, threats, cp, wdl in dl:
            us_i, us_o = us_i.to(device), us_o.to(device)
            th_i, th_o = th_i.to(device), th_o.to(device)
            threats = threats.to(device)
            cp, wdl = cp.to(device), wdl.to(device)

            pred = model(us_i, us_o, th_i, th_o, threats)
            p = torch.sigmoid(pred)                       # predicted win prob
            q = torch.sigmoid(cp / NNUE_SCALE)            # teacher win prob

            has_wdl = wdl >= 0.0
            # blend teacher eval with the real game outcome where we have it
            target = torch.where(has_wdl, args.lam * q + (1.0 - args.lam) * wdl, q)
            loss = torch.nn.functional.mse_loss(p, target)

            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            total += loss.item()
            count += 1

        model.eval()
        vtotal = vcount = 0
        with torch.no_grad():
            for us_i, us_o, th_i, th_o, threats, cp, wdl in vdl:
                pred = model(us_i.to(device), us_o.to(device), th_i.to(device), th_o.to(device), threats.to(device))
                q = torch.sigmoid(cp.to(device) / NNUE_SCALE)
                vtotal += torch.nn.functional.mse_loss(torch.sigmoid(pred), q).item()
                vcount += 1
        sched.step()

        tr = total / max(1, count)
        va = vtotal / max(1, vcount)
        print(f"epoch {epoch:3d}  train {tr:.6f}  val {va:.6f}  lr {sched.get_last_lr()[0]:.2e}")
        # gate on VALIDATION loss - a train loss of 0.0 just means memorised
        if va < best:
            best = va
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            torch.save(model.state_dict(), args.out)

    model.load_state_dict(torch.load(args.out))
    export_binary(model, args.out.replace(".pt", ".nnue"))


def export_binary(model, path):
    ft_w = np.clip(model.ft.weight.detach().cpu().numpy() * QA, -32767, 32767).astype(np.int16)
    ft_b = np.clip(model.ft_bias.detach().cpu().numpy() * QA, -32767, 32767).astype(np.int16)
    # C++ reads l1_weights_[in * HT2 + out] -> transpose from (out, in)
    l1_w = np.clip(model.l1.weight.detach().cpu().numpy().T * QA, -32767, 32767).astype(np.int16)
    l1_b = (model.l1.bias.detach().cpu().numpy() * QA * QB).astype(np.int32)
    l2_w = np.clip(model.l2.weight.detach().cpu().numpy().flatten() * QB, -32767, 32767).astype(np.int16)
    l2_b = np.int32(model.l2.bias.detach().cpu().numpy()[0] * QA * QB)

    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIIIIII", FT_SIZE, HT1, HT2, QA, QB, 1, THREAT_INPUTS))  # stm relative + threat inputs
        f.write(ft_w.tobytes())
        f.write(ft_b.tobytes())
        f.write(l1_w.tobytes())
        f.write(l1_b.tobytes())
        f.write(l2_w.tobytes())
        f.write(np.int32(l2_b).tobytes())
    print(f"exported {path} ({Path(path).stat().st_size / 1024:.0f} KiB)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", nargs="+", default=["datasets/diverse_sf18.jsonl"])
    ap.add_argument("--out", default="networks/nnue.pt")
    ap.add_argument("--batch", type=int, default=8192)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--lam", type=float, default=0.7, help="weight on teacher eval vs game result")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=2)
    args = ap.parse_args()
    train(args)
