#!/usr/bin/env python3
"""Tune then independently validate NNUE blend strength against classical eval.

The NNUE is promoted only if a held-out match passes. Tuning and validation
matches use fresh processes, paired colors/openings, and disabled experience
books on both sides.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

import chess
import chess.engine

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENGINE = ROOT / "bin" / "chess_engine"
OPENINGS = [
    [], ["e2e4", "e7e5"], ["d2d4", "d7d5"], ["c2c4", "e7e5"],
    ["g1f3", "d7d5"], ["e2e4", "c7c5"], ["d2d4", "g8f6"],
    ["e2e4", "e7e6"], ["c2c4", "c7c5"], ["g1f3", "g8f6"],
    ["e2e4", "c7c6"], ["d2d4", "d7d5", "c2c4", "e7e6"],
    ["e2e4", "e7e5", "g1f3", "b8c6"],
    ["d2d4", "g8f6", "c2c4", "g7g6"],
    ["e2e4", "c7c5", "g1f3", "d7d6"],
    ["g1f3", "d7d5", "g2g3", "g8f6"],
]


def run_match(engine_path: Path, candidate: Path, blend: int, games: int,
              move_time: float, max_plies: int, seed: int) -> dict:
    rng = random.Random(seed)
    openings = [list(opening) for opening in OPENINGS]
    rng.shuffle(openings)
    limit = chess.engine.Limit(time=move_time)
    candidate_points = 0.0
    wins = draws = losses = 0
    game_results = []
    candidate_engine = champion_engine = None

    try:
        candidate_engine = chess.engine.SimpleEngine.popen_uci(str(engine_path))
        champion_engine = chess.engine.SimpleEngine.popen_uci(str(engine_path))
        candidate_engine.configure({
            "Threads": 1,
            "Hash": 64,
            "OwnBook": False,
            "NNUEFile": str(candidate.resolve()),
            "NNUEBlend": int(blend),
        })
        champion_engine.configure({"Threads": 1, "Hash": 64, "OwnBook": False})

        for index in range(games):
            # Each adjacent pair uses the same opening with candidate colors swapped.
            opening_moves = openings[(index // 2) % len(openings)]
            candidate_white = index % 2 == 0
            board = chess.Board()
            for uci in opening_moves:
                board.push_uci(uci)

            # A fresh game token forces UCI ucinewgame on both processes so
            # transposition tables from a prior game cannot bleed into the next.
            game_token = object()
            while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
                candidate_turn = (board.turn == chess.WHITE) == candidate_white
                engine = candidate_engine if candidate_turn else champion_engine
                move = engine.play(board, limit, game=game_token).move
                if move is None or move not in board.legal_moves:
                    raise RuntimeError(f"Illegal/missing move in game {index + 1}")
                board.push(move)

            outcome = board.outcome(claim_draw=True)
            points = 0.5
            result_text = "draw"
            if outcome is not None and outcome.winner is not None:
                won = outcome.winner == candidate_white
                result_text = "win" if won else "loss"
                points = 1.0 if won else 0.0
            candidate_points += points
            if points == 1.0:
                wins += 1
            elif points == 0.5:
                draws += 1
            else:
                losses += 1
            game_results.append({
                "game": index + 1,
                "opening_uci": opening_moves,
                "candidate_color": "white" if candidate_white else "black",
                "result_for_candidate": result_text,
                "plies": board.ply(),
            })
            print(
                f"[blend {blend}%] [{index + 1}/{games}] {result_text}; "
                f"{wins}W {draws}D {losses}L",
                flush=True,
            )
    finally:
        for engine in (candidate_engine, champion_engine):
            if engine is not None:
                try:
                    engine.quit()
                except Exception:
                    pass

    return {
        "blend_percent": int(blend),
        "games": games,
        "score_fraction": candidate_points / games,
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "game_results": game_results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=DEFAULT_ENGINE)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--games", type=int, default=40, help="held-out validation games (even)")
    parser.add_argument("--tune-games", type=int, default=16, help="even games per blend for tuning")
    parser.add_argument("--blend-candidates", type=str, default="15,30,45,70")
    parser.add_argument("--move-time", type=float, default=0.15)
    parser.add_argument("--min-score", type=float, default=0.55)
    parser.add_argument("--max-plies", type=int, default=200)
    parser.add_argument("--seed", type=int, default=19019)
    parser.add_argument("--report", type=Path, default=ROOT / "networks" / "candidate_validation.json")
    args = parser.parse_args()

    if args.games < 2 or args.games % 2:
        parser.error("--games must be an even integer >= 2")
    if args.tune_games < 2 or args.tune_games % 2:
        parser.error("--tune-games must be an even integer >= 2")
    if not 0.5 <= args.min_score <= 1.0:
        parser.error("--min-score must be between 0.5 and 1.0")
    if not args.engine.is_file():
        parser.error(f"engine binary not found: {args.engine}")
    if not args.candidate.is_file() or args.candidate.stat().st_size == 0:
        parser.error(f"candidate network missing or empty: {args.candidate}")
    try:
        blends = sorted({int(value.strip()) for value in args.blend_candidates.split(",")})
    except ValueError:
        parser.error("--blend-candidates must be comma-separated integers")
    if not blends or any(value < 1 or value > 100 for value in blends):
        parser.error("blend candidates must be between 1 and 100")

    tuning = []
    for blend in blends:
        tuning.append(run_match(
            args.engine, args.candidate, blend, args.tune_games,
            args.move_time, args.max_plies, args.seed,
        ))

    # If results tie, prefer the more conservative neural contribution.
    best = max(tuning, key=lambda row: (row["score_fraction"], -row["blend_percent"]))
    final_seed = args.seed + 101
    final = run_match(
        args.engine, args.candidate, best["blend_percent"], args.games,
        args.move_time, args.max_plies, final_seed,
    )
    passed = final["score_fraction"] >= args.min_score
    report = {
        "status": "PASS" if passed else "REJECT",
        "candidate": str(args.candidate),
        "champion": "default classical evaluation",
        "tuned_blend_percent": best["blend_percent"],
        "tuning_matches": [
            {key: value for key, value in row.items() if key != "game_results"}
            for row in tuning
        ],
        "games": args.games,
        "tune_games_per_blend": args.tune_games,
        "move_time_seconds": args.move_time,
        "minimum_score_fraction": args.min_score,
        "candidate_score_fraction": round(final["score_fraction"], 4),
        "wins": final["wins"],
        "draws": final["draws"],
        "losses": final["losses"],
        "seed": final_seed,
        "note": (
            "Blend was tuned on screening games; held-out validation is a short gate, "
            "not statistical proof. Confirm promising candidates with a longer paired SPRT match."
        ),
        "game_results": final["game_results"],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "game_results"}, indent=2))
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
