#!/usr/bin/env python3
"""Generate diverse, engine-approved opening positions for paired-color matches."""
from __future__ import annotations

import argparse
import json
import math
import random
from pathlib import Path

import chess
import chess.engine


def choose_opening_move(engine, board: chess.Board, rng: random.Random, depth: int, multipv: int, score_band: int):
    infos = engine.analyse(board, chess.engine.Limit(depth=depth), multipv=multipv)
    if isinstance(infos, dict):
        infos = [infos]
    scored = []
    for info in infos:
        pv = info.get("pv") or []
        score = info.get("score")
        if not pv or score is None:
            continue
        cp = score.pov(board.turn).score(mate_score=10000)
        if cp is not None and pv[0] in board.legal_moves:
            scored.append((int(cp), pv[0]))
    if not scored:
        raise RuntimeError("Stockfish returned no usable opening candidate")
    best = max(score for score, _ in scored)
    eligible = [(score, move) for score, move in scored if best - score <= score_band]
    weights = [math.exp(max(-20.0, (score - best) / 100.0)) for score, _ in eligible]
    return rng.choices([move for _, move in eligible], weights=weights, k=1)[0]


def generate(stockfish: Path, out: Path, count: int, seed: int, depth: int, multipv: int,
             plies_min: int, plies_max: int, score_band: int) -> list[dict]:
    if count < 1 or depth < 6 or multipv < 2 or plies_min < 8 or plies_min % 2 or plies_max < plies_min or plies_max % 2:
        raise ValueError("invalid count/depth/MultiPV/ply settings")
    rng = random.Random(seed)
    engine = chess.engine.SimpleEngine.popen_uci(str(stockfish))
    rows: list[dict] = []
    seen: set[str] = set()
    attempts = 0
    try:
        engine.configure({"Threads": 1, "Hash": 128})
        while len(rows) < count and attempts < count * 30:
            attempts += 1
            target_plies = rng.choice(list(range(plies_min, plies_max + 1, 2)))
            board = chess.Board()
            moves = []
            try:
                for _ in range(target_plies):
                    move = choose_opening_move(engine, board, rng, depth, multipv, score_band)
                    moves.append(move.uci())
                    board.push(move)
            except Exception:
                continue
            if board.turn != chess.WHITE or board.is_game_over(claim_draw=True):
                continue
            fen = board.fen()
            key = " ".join(fen.split()[:4])
            if key in seen:
                continue
            seen.add(key)
            rows.append({
                "index": len(rows),
                "moves": " ".join(moves),
                "fen": fen,
                "plies": len(moves),
                "generator": "stockfish19-multipv",
                "depth": depth,
                "multipv": multipv,
                "seed": seed,
            })
            if len(rows) % 50 == 0:
                print(f"opening positions generated {len(rows)}/{count}", flush=True)
    finally:
        engine.quit()
    if len(rows) != count:
        raise RuntimeError(f"only generated {len(rows)}/{count} unique opening positions")
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
    print(f"Saved {len(rows)} unique, paired-color-ready opening positions to {out}", flush=True)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stockfish", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=500)
    parser.add_argument("--seed", type=int, default=20261101)
    parser.add_argument("--depth", type=int, default=9)
    parser.add_argument("--multipv", type=int, default=4)
    parser.add_argument("--plies-min", type=int, default=8)
    parser.add_argument("--plies-max", type=int, default=16)
    parser.add_argument("--score-band", type=int, default=120)
    args = parser.parse_args()
    generate(args.stockfish, args.out, args.count, args.seed, args.depth, args.multipv,
             args.plies_min, args.plies_max, args.score_band)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
