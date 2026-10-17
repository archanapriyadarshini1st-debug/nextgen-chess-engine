#!/usr/bin/env python3
"""Validate a newly trained NNUE against the current classical-evaluation champion.

The candidate is promoted only when it scores at least the configured threshold
in paired, color-swapped games. This is a practical smoke gate, not a claim of
statistical proof or a replacement for a long SPRT tournament.
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

import chess
import chess.engine
import chess.pgn

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENGINE = ROOT / "bin" / "chess_engine"
OPENINGS = [
    [],
    ["e2e4", "e7e5"],
    ["d2d4", "d7d5"],
    ["c2c4", "e7e5"],
    ["g1f3", "d7d5"],
    ["e2e4", "c7c5"],
    ["d2d4", "g8f6"],
    ["e2e4", "e7e6"],
    ["c2c4", "c7c5"],
    ["g1f3", "g8f6"],
    ["e2e4", "c7c6"],
    ["d2d4", "d7d5", "c2c4", "e7e6"],
    ["e2e4", "e7e5", "g1f3", "b8c6"],
    ["d2d4", "g8f6", "c2c4", "g7g6"],
    ["e2e4", "c7c5", "g1f3", "d7d6"],
    ["g1f3", "d7d5", "g2g3", "g8f6"],
]


def play_one(engine, board: chess.Board, limit: chess.engine.Limit, max_plies: int) -> chess.Outcome:
    while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
        result = engine.play(board, limit)
        if result.move is None or result.move not in board.legal_moves:
            raise RuntimeError(f"Engine returned an illegal or missing move at {board.fen()}")
        board.push(result.move)
    outcome = board.outcome(claim_draw=True)
    if outcome is None:
        # Treat an unusually long unfinished game as a draw, consistently.
        return chess.Outcome(chess.Termination.FIFTY_MOVES, None)
    return outcome


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", type=Path, default=DEFAULT_ENGINE)
    ap.add_argument("--candidate", type=Path, required=True, help="candidate NNUE network")
    ap.add_argument("--games", type=int, default=40, help="even number of games (default: 40)")
    ap.add_argument("--move-time", type=float, default=0.15, help="seconds per move for both sides")
    ap.add_argument("--min-score", type=float, default=0.55, help="candidate score fraction required to pass")
    ap.add_argument("--max-plies", type=int, default=200)
    ap.add_argument("--seed", type=int, default=19019)
    ap.add_argument("--report", type=Path, default=ROOT / "networks" / "candidate_validation.json")
    args = ap.parse_args()

    if args.games < 2 or args.games % 2:
        ap.error("--games must be an even integer >= 2")
    if not 0.5 <= args.min_score <= 1.0:
        ap.error("--min-score must be between 0.5 and 1.0")
    if not args.engine.is_file():
        ap.error(f"engine binary not found: {args.engine}")
    if not args.candidate.is_file() or args.candidate.stat().st_size == 0:
        ap.error(f"candidate network missing or empty: {args.candidate}")

    rng = random.Random(args.seed)
    openings = OPENINGS.copy()
    rng.shuffle(openings)
    limit = chess.engine.Limit(time=args.move_time)
    candidate_score = 0.0
    wins = draws = losses = 0
    games = []
    candidate_engine = champion_engine = None

    try:
        candidate_engine = chess.engine.SimpleEngine.popen_uci(str(args.engine))
        champion_engine = chess.engine.SimpleEngine.popen_uci(str(args.engine))
        # Disable the experience book in both engines: otherwise both can replay
        # Stockfish-labelled moves, making the NNUE comparison meaningless.
        candidate_engine.configure({
            "Threads": 1, "Hash": 64, "OwnBook": False,
            "NNUEFile": str(args.candidate.resolve()),
        })
        # No NNUEFile means the opt-in classical evaluator is the champion.
        champion_engine.configure({"Threads": 1, "Hash": 64, "OwnBook": False})
        for index in range(args.games):
            opening_moves = openings[index % len(openings)]
            candidate_white = (index % 2 == 0)
            board = chess.Board()
            for uci in opening_moves:
                board.push_uci(uci)
            # Color swaps are paired by reusing the same opening on adjacent games.
            if index % 2 == 1:
                opening_moves = openings[(index - 1) % len(openings)]
                board = chess.Board()
                for uci in opening_moves:
                    board.push_uci(uci)

            candidate_is_white = candidate_white
            ply = 0
            while not board.is_game_over(claim_draw=True) and board.ply() < args.max_plies:
                is_candidate_turn = (board.turn == chess.WHITE) == candidate_is_white
                engine = candidate_engine if is_candidate_turn else champion_engine
                result = engine.play(board, limit)
                if result.move is None or result.move not in board.legal_moves:
                    raise RuntimeError(f"Illegal/missing move at game {index + 1}: {board.fen()}")
                board.push(result.move)
                ply += 1

            outcome = board.outcome(claim_draw=True)
            result_text = "draw"
            score = 0.5
            if outcome is not None and outcome.winner is not None:
                candidate_won = outcome.winner == candidate_is_white
                result_text = "win" if candidate_won else "loss"
                score = 1.0 if candidate_won else 0.0
            candidate_score += score
            if score == 1.0:
                wins += 1
            elif score == 0.5:
                draws += 1
            else:
                losses += 1
            games.append({
                "game": index + 1,
                "opening_uci": opening_moves,
                "candidate_color": "white" if candidate_is_white else "black",
                "result_for_candidate": result_text,
                "plies": ply,
            })
            print(f"[{index + 1}/{args.games}] candidate {result_text} ({ply} plies)", flush=True)
    finally:
        for engine in (candidate_engine, champion_engine):
            if engine is not None:
                try:
                    engine.quit()
                except Exception:
                    pass

    score_fraction = candidate_score / args.games
    passed = score_fraction >= args.min_score
    report = {
        "status": "PASS" if passed else "REJECT",
        "candidate": str(args.candidate),
        "champion": "default classical evaluation",
        "games": args.games,
        "move_time_seconds": args.move_time,
        "minimum_score_fraction": args.min_score,
        "candidate_score_fraction": round(score_fraction, 4),
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "seed": args.seed,
        "note": "Short promotion gate only; confirm promising candidates with a longer paired SPRT match.",
        "game_results": games,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "game_results"}, indent=2))
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
