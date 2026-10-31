#!/usr/bin/env python3
"""Build a balanced Stockfish value-label dataset from whole games and mistakes.

The NNUE is an evaluation model, not a move-prediction model. Most training
examples should therefore represent ordinary positions, not only positions
where the current engine blundered. A small critical subset receives deeper
analysis at depth 20, with a capped depth-28 pass for the highest-impact cases.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

import chess
import chess.engine
import chess.pgn


def position_key(fen: str) -> str:
    return " ".join(fen.split()[:4])


def sample_game_positions(pgn_path: Path, every_plies: int, max_positions: int, seed: int):
    candidates: dict[str, str] = {}
    games = 0
    with pgn_path.open("r", encoding="utf-8", errors="replace") as stream:
        while True:
            game = chess.pgn.read_game(stream)
            if game is None:
                break
            games += 1
            board = game.board()
            for ply, move in enumerate(game.mainline_moves()):
                # Avoid making the broad set almost entirely opening positions.
                if ply >= 4 and ply % every_plies == 0:
                    fen = board.fen()
                    candidates.setdefault(position_key(fen), fen)
                board.push(move)

    keys = sorted(candidates)
    rng = random.Random(seed)
    selected = rng.sample(keys, min(max_positions, len(keys)))
    return [candidates[key] for key in selected], games, len(candidates)


def critical_positions(missed_path: Path, count: int) -> list[tuple[str, int]]:
    if not missed_path.exists() or count <= 0:
        return []
    parsed: list[tuple[int, str, str]] = []
    for line in missed_path.read_text(encoding="utf-8").splitlines():
        try:
            row = json.loads(line)
            fen = str(row["fen"])
            key = position_key(fen)
            loss = int(row.get("cp_loss", 0))
            if loss >= 50:
                parsed.append((loss, key, fen))
        except Exception:
            continue
    parsed.sort(reverse=True)
    rows: list[tuple[str, int]] = []
    seen: set[str] = set()
    for loss, key, fen in parsed:
        if key in seen:
            continue
        try:
            chess.Board(fen)
        except Exception:
            continue
        seen.add(key)
        rows.append((fen, loss))
        if len(rows) >= count:
            break
    return rows


def analyse_positions(engine, positions: list[tuple[str, int, str]], output, time_limits: dict[int, float]) -> int:
    written = 0
    for index, (fen, requested_depth, label_source) in enumerate(positions, start=1):
        board = chess.Board(fen)
        try:
            # A wall-time cap protects the 6-hour hosted runner from rare,
            # exceptionally difficult searches. Actual depth is recorded.
            limit = chess.engine.Limit(
                depth=requested_depth,
                time=time_limits.get(requested_depth),
            )
            info = engine.analyse(board, limit)
            score = info["score"].pov(board.turn).score(mate_score=10000)
            if score is None:
                continue
            pv = info.get("pv") or []
            row = {
                "fen": fen,
                "best_cp": int(score),
                "best": pv[0].uci() if pv else None,
                "teacher_depth": int(info.get("depth") or requested_depth),
                "requested_depth": requested_depth,
                "label_source": label_source,
            }
            output.write(json.dumps(row, ensure_ascii=False) + "\n")
            output.flush()
            written += 1
        except Exception as exc:
            print(f"warning: Stockfish analysis skipped position {index}: {exc}", flush=True)
        if index % 100 == 0 or index == len(positions):
            print(f"teacher labels {index}/{len(positions)}; saved={written}", flush=True)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgn", type=Path, required=True)
    parser.add_argument("--missed", type=Path, required=True)
    parser.add_argument("--stockfish", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sample-every-plies", type=int, default=8)
    parser.add_argument("--max-positions", type=int, default=1200)
    parser.add_argument("--depth", type=int, default=12)
    parser.add_argument("--critical-count", type=int, default=100)
    parser.add_argument("--critical-depth", type=int, default=20)
    parser.add_argument("--elite-count", type=int, default=8)
    parser.add_argument("--elite-depth", type=int, default=28)
    parser.add_argument("--bulk-time-limit", type=float, default=2.0)
    parser.add_argument("--critical-time-limit", type=float, default=8.0)
    parser.add_argument("--elite-time-limit", type=float, default=20.0)
    parser.add_argument("--seed", type=int, default=19019)
    args = parser.parse_args()

    if args.sample_every_plies < 1 or args.max_positions < 1:
        parser.error("sampling interval and maximum positions must be positive")
    if not (args.depth <= args.critical_depth <= args.elite_depth):
        parser.error("depths must be ordered bulk <= critical <= elite")
    if args.critical_count < 1 or args.elite_count < 0:
        parser.error("critical-count must be positive and elite-count nonnegative")
    if not args.stockfish.is_file():
        parser.error(f"Stockfish binary does not exist: {args.stockfish}")
    if not args.pgn.is_file():
        parser.error(f"PGN does not exist: {args.pgn}")

    broad, game_count, unique_candidates = sample_game_positions(
        args.pgn, args.sample_every_plies, args.max_positions, args.seed
    )
    critical = critical_positions(args.missed, args.critical_count)
    critical_source = "critical_mistake"
    if not critical and args.critical_count > 0:
        # A clean clock-match dataset has no precomputed mistake labels. Pick a
        # deterministic subset of its broad positions for deeper teacher passes.
        rng = random.Random(args.seed + 24)
        count = min(args.critical_count, len(broad))
        critical = [(fen, 0) for fen in rng.sample(broad, count)]
        critical_source = "deep_broad_sample"
    elite = critical[: min(args.elite_count, len(critical))]

    tasks: list[tuple[str, int, str]] = []
    tasks.extend((fen, args.depth, "broad_pgn") for fen in broad)
    tasks.extend((fen, args.critical_depth, critical_source) for fen, _ in critical)
    tasks.extend((fen, args.elite_depth, "elite_" + critical_source) for fen, _ in elite)
    if not tasks:
        raise SystemExit("No positions were selected from PGN or missed-move data.")

    # Write/flush every label so already completed analysis is recoverable if
    # a runner is interrupted. An individual Stockfish error doesn't kill a batch.
    args.out.parent.mkdir(parents=True, exist_ok=True)
    engine = chess.engine.SimpleEngine.popen_uci(str(args.stockfish))
    try:
        try:
            engine.configure({"Threads": 2, "Hash": 128})
        except Exception:
            pass
        with args.out.open("w", encoding="utf-8") as output:
            saved = analyse_positions(
                engine,
                tasks,
                output,
                {
                    args.depth: args.bulk_time_limit,
                    args.critical_depth: args.critical_time_limit,
                    args.elite_depth: args.elite_time_limit,
                },
            )
    finally:
        try:
            engine.quit()
        except Exception:
            pass

    if saved < 100:
        raise SystemExit(f"Only {saved} usable labels were produced; refusing low-coverage dataset.")
    print(json.dumps({
        "pgn_games_read": game_count,
        "distinct_sampled_position_candidates": unique_candidates,
        "broad_positions_requested": len(broad),
        "critical_positions_requested": len(critical),
        "elite_positions_requested": len(elite),
        "labels_saved": saved,
        "bulk_depth": args.depth,
        "critical_depth": args.critical_depth,
        "elite_max_depth": args.elite_depth,
        "output": str(args.out),
    }, indent=2))


if __name__ == "__main__":
    main()
