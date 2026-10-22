#!/usr/bin/env python3
"""Clock-controlled, paired-color matches: NextGen vs Stockfish 19.

Use --tc 600+0 for genuine 10-minutes-per-side, zero-increment chess.
Stockfish is MAX strength when --sf-elo 0; no depth limit is passed during
clock-controlled play. Timed UCI searches choose their own depth.

The tournament runner deliberately disables NextGen's experience book by
default. Training hints must not leak into an independent strength test.
"""
from __future__ import annotations

import argparse
import json
import math
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path

import chess
import chess.engine
import chess.pgn

ROOT = Path(__file__).resolve().parent.parent
OUR = ROOT / "bin" / "chess_engine"


def _find_sf() -> Path:
    for path in (
        ROOT / "bin" / "stockfish19",
        Path("/tmp/sf19/stockfish19"),
        Path("/tmp/stockfish19"),
    ):
        if path.exists():
            return path
    return ROOT / "bin" / "stockfish19"


SF = _find_sf()
DATA = ROOT / "datasets"

OPENINGS = [
    "",
    "e2e4 e7e5",
    "e2e4 c7c5",
    "e2e4 e7e6",
    "e2e4 c7c6",
    "d2d4 d7d5",
    "d2d4 g8f6",
    "c2c4 e7e5",
    "g1f3 d7d5",
    "e2e4 e7e5 g1f3 b8c6 f1b5",
    "e2e4 c7c5 g1f3 d7d6 d2d4",
    "d2d4 g8f6 c2c4 e7e6",
    "d2d4 d7d5 c2c4 c7c6",
    "e2e4 g8f6",
    "e2e4 e7e5 g1f3 g8f6",
]


def parse_tc(value: str) -> tuple[float, float]:
    """Parse seconds-per-side plus increment, e.g. 600+0 means 10+0."""
    try:
        if "+" in value:
            base_text, inc_text = value.split("+", 1)
            base, inc = float(base_text), float(inc_text)
        else:
            base, inc = float(value), 0.0
    except (ValueError, TypeError) as exc:
        raise argparse.ArgumentTypeError("time control must be seconds or base+increment, e.g. 600+0") from exc
    if base <= 0 or inc < 0:
        raise argparse.ArgumentTypeError("base time must be > 0 and increment must be >= 0")
    return base, inc


def elo_from_score(opponent_elo: int, score: float, games: int) -> float | None:
    """Return a rough performance estimate only when opponent Elo is calibrated."""
    if games <= 0:
        return None
    # Small correction prevents infinite ratings for a 0% or 100% short match.
    p = (score * games + 0.5) / (games + 1.0)
    p = min(max(p, 1e-6), 1 - 1e-6)
    return opponent_elo - 400.0 * math.log10(1.0 / p - 1.0)


def start_our(use_experience_book: bool, hash_mb: int):
    engine = chess.engine.SimpleEngine.popen_uci(str(OUR))
    engine.configure({"Hash": hash_mb, "Threads": 1, "OwnBook": bool(use_experience_book)})
    if use_experience_book:
        book = ROOT / "datasets" / "experience_book.txt"
        if book.exists():
            engine.configure({"ExperienceFile": str(book), "OwnBook": True})
    return engine


def start_sf(elo: int | None, hash_mb: int):
    engine = chess.engine.SimpleEngine.popen_uci(str(SF))
    options = {"Hash": hash_mb, "Threads": 1}
    if elo is not None:
        options.update({"UCI_LimitStrength": True, "UCI_Elo": int(elo)})
    else:
        # MAX: never set UCI_Elo or a skill cap; allow Stockfish to use the
        # available move clock and its own iterative-deepening decisions.
        options["UCI_LimitStrength"] = False
    engine.configure(options)
    return engine


def safe_quit(engine) -> None:
    if engine is None:
        return
    try:
        engine.quit()
    except Exception:
        try:
            engine.protocol.process.kill()
        except Exception:
            pass


def play(our, sf, our_white: bool, opening: str, base: float, inc: float, max_ply: int = 240) -> dict:
    board = chess.Board()
    if opening:
        for token in opening.split():
            board.push_uci(token)

    white_clock = black_clock = base
    moves: list[str] = []
    crashed = None

    while not board.is_game_over(claim_draw=True) and board.ply() < max_ply:
        is_our = (board.turn == chess.WHITE) == our_white
        engine = our if is_our else sf
        limit = chess.engine.Limit(
            white_clock=max(0.01, white_clock),
            black_clock=max(0.01, black_clock),
            white_inc=inc,
            black_inc=inc,
        )
        started = time.monotonic()
        try:
            response = engine.play(board, limit)
        except Exception as exc:
            crashed = f"{'nextgen' if is_our else 'sf19'}: {exc}"
            break

        elapsed = time.monotonic() - started
        if response.move is None or response.move not in board.legal_moves:
            crashed = f"{'nextgen' if is_our else 'sf19'} returned illegal/null move {response.move}"
            break

        if board.turn == chess.WHITE:
            white_clock = max(0.0, white_clock - elapsed + inc)
        else:
            black_clock = max(0.0, black_clock - elapsed + inc)
        moves.append(response.move.uci())
        board.push(response.move)

        if white_clock <= 0.0 or black_clock <= 0.0:
            break

    if crashed:
        result = ("0-1" if our_white else "1-0") if crashed.startswith("nextgen") else (
            "1-0" if our_white else "0-1"
        )
        termination = "crash:" + crashed
    elif white_clock <= 0.0:
        result, termination = "0-1", "white_flag"
    elif black_clock <= 0.0:
        result, termination = "1-0", "black_flag"
    elif board.is_game_over(claim_draw=True):
        result = board.result(claim_draw=True)
        outcome = board.outcome(claim_draw=True)
        termination = outcome.termination.name if outcome else "game_over"
    else:
        result, termination = "1/2-1/2", "max_ply"

    if result == "1-0":
        our_result = "win" if our_white else "loss"
    elif result == "0-1":
        our_result = "loss" if our_white else "win"
    else:
        our_result = "draw"

    return {
        "result": result,
        "our_result": our_result,
        "our_white": our_white,
        "opening": opening,
        "ply": board.ply(),
        "termination": termination,
        "moves": moves,
        "final_fen": board.fen(),
        "white_time_left_seconds": round(white_clock, 3),
        "black_time_left_seconds": round(black_clock, 3),
    }


def write_pgn(path: Path, game_data: dict, *, round_no: int, tc: str, sf_label: str) -> None:
    """Persist full, legal mainline games for post-game weakness analysis."""
    game = chess.pgn.Game()
    board = game.board()
    node = game

    for token in (game_data.get("opening") or "").split():
        move = chess.Move.from_uci(token)
        if move not in board.legal_moves:
            raise ValueError(f"Illegal move in configured opening: {token}")
        node = node.add_variation(move)
        board.push(move)

    for token in game_data["moves"]:
        move = chess.Move.from_uci(token)
        if move not in board.legal_moves:
            raise ValueError(f"Recorded match contains illegal move: {token}")
        node = node.add_variation(move)
        board.push(move)

    game.headers["Event"] = "NextGen strict clock match"
    game.headers["Site"] = "GitHub Actions"
    game.headers["Date"] = datetime.now(timezone.utc).strftime("%Y.%m.%d")
    game.headers["Round"] = str(round_no)
    game.headers["White"] = "NextGen" if game_data["our_white"] else f"Stockfish 19 {sf_label}"
    game.headers["Black"] = f"Stockfish 19 {sf_label}" if game_data["our_white"] else "NextGen"
    game.headers["Result"] = game_data["result"]
    game.headers["TimeControl"] = tc
    game.headers["Termination"] = game_data["termination"]
    game.headers["OpeningUCI"] = game_data.get("opening", "")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        print(game, file=stream, end="\n\n")
        stream.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--games", type=int, default=8, help="even number; paired colors/openings")
    parser.add_argument("--tc", default="600+0", help="seconds per side + increment; 600+0 = true 10+0")
    parser.add_argument("--sf-elo", type=int, default=0, help="0 = Stockfish 19 MAX (no Elo cap)")
    parser.add_argument("--tag", default="strict-10plus0")
    parser.add_argument("--hash-mb", type=int, default=256, help="equal Hash budget for both engines")
    parser.add_argument("--use-experience-book", action="store_true", help="not recommended for independent strength tests")
    parser.add_argument("--jsonl", type=Path, default=DATA / "clock_match.jsonl")
    parser.add_argument("--pgn", type=Path, default=DATA / "clock_match.pgn")
    parser.add_argument("--summary", type=Path, default=DATA / "clock_match_summary.json")
    parser.add_argument("--max-ply", type=int, default=240)
    args = parser.parse_args()

    if args.games < 2 or args.games % 2:
        parser.error("--games must be an even integer >= 2 so colors can be paired")
    if args.hash_mb < 16:
        parser.error("--hash-mb must be >= 16 MB")
    try:
        base, inc = parse_tc(args.tc)
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))

    opponent_elo = args.sf_elo if args.sf_elo > 0 else None
    sf_label = f"Elo{opponent_elo}" if opponent_elo is not None else "MAX"
    tc_header = f"{base:g}+{inc:g}"
    print(
        f"=== NextGen vs Stockfish 19 {sf_label} | TC {tc_header} "
        f"({base:g}s + {inc:g}s) | games={args.games} | paired colors/openings ===",
        flush=True,
    )
    print("NextGen book: " + ("enabled (not rating-clean)" if args.use_experience_book else "DISABLED"),
          flush=True)
    print(f"Equal engine resources: Threads=1, Hash={args.hash_mb} MB each", flush=True)
    print("Clock-managed UCI search; no fixed depth is passed to either engine.", flush=True)

    args.jsonl.parent.mkdir(parents=True, exist_ok=True)
    args.pgn.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)

    wins = losses = draws = discarded = 0
    our = sf = None

    def restart():
        nonlocal our, sf
        safe_quit(our)
        safe_quit(sf)
        time.sleep(0.2)
        our = start_our(args.use_experience_book, args.hash_mb)
        sf = start_sf(opponent_elo, args.hash_mb)
        return our, sf

    started_all = time.monotonic()
    restart()
    try:
        for index in range(args.games):
            our_white = (index % 2 == 0)
            opening = OPENINGS[(index // 2) % len(OPENINGS)]
            pair = index // 2 + 1
            try:
                game_data = play(our, sf, our_white, opening, base, inc, args.max_ply)
            except Exception as exc:
                print(f"  hard failure in game {index + 1}: {exc}", flush=True)
                traceback.print_exc()
                discarded += 1
                restart()
                continue

            if game_data["our_result"] == "win":
                wins += 1
            elif game_data["our_result"] == "loss":
                losses += 1
            else:
                draws += 1

            record = {
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "tag": args.tag,
                "time_control": tc_header,
                "stockfish": sf_label,
                "hash_mb_each": args.hash_mb,
                "threads_each": 1,
                "paired_opening_id": pair,
                **{key: game_data[key] for key in (
                    "result", "our_result", "our_white", "opening", "ply",
                    "termination", "white_time_left_seconds", "black_time_left_seconds",
                )},
            }
            with args.jsonl.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
                stream.flush()
            write_pgn(args.pgn, game_data, round_no=index + 1, tc=tc_header, sf_label=sf_label)

            played = wins + losses + draws
            score = (wins + 0.5 * draws) / played if played else 0.0
            print(
                f"  {index + 1}/{args.games} pair={pair} "
                f"{'W' if our_white else 'B'} {game_data['our_result']:4s} "
                f"ply={game_data['ply']:3d} {game_data['termination'][:20]:20s} "
                f"{wins}W {draws}D {losses}L score={score:.3f}",
                flush=True,
            )
            if (index + 1) % 8 == 0 and index + 1 < args.games:
                restart()
    finally:
        safe_quit(our)
        safe_quit(sf)

    games_played = wins + losses + draws
    score = (wins + 0.5 * draws) / games_played if games_played else 0.0
    total_seconds = time.monotonic() - started_all
    summary = {
        "tag": args.tag,
        "time_control_seconds": base,
        "increment_seconds": inc,
        "sf": sf_label,
        "sf_strength_capped": opponent_elo is not None,
        "games": games_played,
        "requested_games": args.games,
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "discarded": discarded,
        "score_fraction": score,
        "hash_mb_each": args.hash_mb,
        "threads_each": 1,
        "paired_colors_and_openings": True,
        "experience_book_enabled": args.use_experience_book,
        "depth_limit_passed": False,
        "elapsed_seconds": round(total_seconds, 2),
        "elo_estimate": elo_from_score(opponent_elo, score, games_played) if opponent_elo is not None else None,
        "rating_note": (
            "No absolute Elo estimate emitted: Stockfish 19 MAX has no validated fixed Elo reference here."
            if opponent_elo is None else
            "Very rough short-match performance estimate only; not a calibrated engine rating."
        ),
    }
    args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("\nFINAL", json.dumps(summary, indent=2), flush=True)
    return 0 if games_played == args.games else 2


if __name__ == "__main__":
    raise SystemExit(main())
