#!/usr/bin/env python3
"""Quality-oriented, paired-color Stockfish clock-match shard runner."""
from __future__ import annotations

import argparse
import json
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import chess

from clock_match import parse_tc, play, safe_quit, start_our, start_sf, write_pgn


def validate_opening_moves(moves: str) -> str:
    tokens = [token for token in moves.split() if token]
    if len(tokens) < 8 or len(tokens) % 2:
        raise ValueError("opening must contain an even number of at least 8 legal plies")
    board = chess.Board()
    for token in tokens:
        try:
            move = chess.Move.from_uci(token)
        except ValueError as exc:
            raise ValueError(f"invalid UCI move in opening: {token}") from exc
        if move not in board.legal_moves:
            raise ValueError(f"illegal opening move: {token}")
        board.push(move)
    if board.is_game_over(claim_draw=True):
        raise ValueError("opening ends the game before the clock match starts")
    if board.turn != chess.WHITE:
        raise ValueError("paired-color opening must leave White to move")
    return board.fen()


def load_openings(path: Path) -> list[dict]:
    openings: list[dict] = []
    seen: set[str] = set()
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
            moves = str(row["moves"])
            fen = validate_opening_moves(moves)
        except Exception as exc:
            raise ValueError(f"invalid opening row {line_number}: {exc}") from exc
        key = " ".join(fen.split()[:4])
        provided_fen = str(row.get("fen", ""))
        if provided_fen and " ".join(provided_fen.split()[:4]) != key:
            raise ValueError(f"opening FEN does not match its move line at row {line_number}")
        if key in seen:
            raise ValueError(f"duplicate opening position at row {line_number}")
        seen.add(key)
        openings.append({"moves": moves, "fen": fen, "key": key})
    if not openings:
        raise ValueError("opening suite is empty")
    return openings


def opening_for_pair(openings: list[dict], offset: int, pair_index: int) -> dict:
    index = offset + pair_index
    if index < 0 or index >= len(openings):
        raise IndexError(f"opening index {index} is outside the {len(openings)}-opening suite")
    return openings[index]


def verify_pairs(records: list[dict]) -> list[str]:
    grouped: dict[int, list[dict]] = defaultdict(list)
    for row in records:
        grouped[int(row["paired_opening_id"])].append(row)
    issues = []
    for pair_id, rows in sorted(grouped.items()):
        if len(rows) != 2:
            issues.append(f"pair {pair_id}: expected 2 games, found {len(rows)}")
            continue
        if {bool(row["our_white"]) for row in rows} != {False, True}:
            issues.append(f"pair {pair_id}: colors were not reversed")
        if len({row["opening"] for row in rows}) != 1:
            issues.append(f"pair {pair_id}: games did not use the same opening")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--games", type=int, default=50)
    parser.add_argument("--tc", default="180+0", help="seconds per side + increment; 180+0 is true 3+0")
    parser.add_argument("--sf-elo", type=int, default=0, help="0 = uncapped Stockfish 19 MAX")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--hash-mb", type=int, default=128)
    parser.add_argument("--openings", type=Path, required=True)
    parser.add_argument("--opening-offset", type=int, default=0)
    parser.add_argument("--jsonl", type=Path, required=True)
    parser.add_argument("--pgn", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--max-ply", type=int, default=400)
    args = parser.parse_args()

    if args.games < 2 or args.games % 2:
        parser.error("--games must be an even integer >= 2")
    if args.hash_mb < 16 or args.opening_offset < 0 or args.max_ply < 40:
        parser.error("hash must be >=16 MB, opening offset nonnegative, and max ply >=40")
    try:
        base, increment = parse_tc(args.tc)
    except Exception as exc:
        parser.error(str(exc))
    openings = load_openings(args.openings)
    required = args.opening_offset + args.games // 2
    if required > len(openings):
        parser.error(f"need {required} opening rows; suite only has {len(openings)}")

    opponent_elo = args.sf_elo if args.sf_elo > 0 else None
    sf_label = f"Elo{opponent_elo}" if opponent_elo is not None else "MAX"
    tc_header = f"{base:g}+{increment:g}"
    for path in (args.jsonl, args.pgn, args.summary):
        path.parent.mkdir(parents=True, exist_ok=True)
    args.jsonl.write_text("", encoding="utf-8")
    args.pgn.write_text("", encoding="utf-8")

    our = sf = None

    def restart():
        nonlocal our, sf
        safe_quit(our)
        safe_quit(sf)
        time.sleep(0.2)
        our = start_our(False, args.hash_mb)
        sf = start_sf(opponent_elo, args.hash_mb)

    wins = draws = losses = 0
    records: list[dict] = []
    started = time.monotonic()
    print(f"Quality shard: {args.tag}; {args.games} paired-color games at {tc_header}", flush=True)
    print(f"Stockfish 19 {sf_label}; Threads=1 and Hash={args.hash_mb} MB for both; NextGen book disabled", flush=True)
    try:
        restart()
        for index in range(args.games):
            pair_index = index // 2
            opening = opening_for_pair(openings, args.opening_offset, pair_index)
            our_white = index % 2 == 0
            game_data = None
            for attempt in range(2):
                candidate = play(our, sf, our_white, opening["moves"], base, increment, args.max_ply)
                if not str(candidate.get("termination", "")).startswith("crash:"):
                    game_data = candidate
                    break
                restart()
            if game_data is None:
                raise RuntimeError(f"pair {args.opening_offset + pair_index + 1} aborted after engine crash")

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
                "experience_book_enabled": False,
                "paired_opening_id": args.opening_offset + pair_index + 1,
                "opening_index": args.opening_offset + pair_index,
                "opening_fen": opening["fen"],
                "result": game_data["result"],
                "our_result": game_data["our_result"],
                "our_white": our_white,
                "opening": opening["moves"],
                "ply": game_data["ply"],
                "termination": game_data["termination"],
                "white_time_left_seconds": game_data["white_time_left_seconds"],
                "black_time_left_seconds": game_data["black_time_left_seconds"],
            }
            with args.jsonl.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
            write_pgn(args.pgn, game_data, round_no=index + 1, tc=tc_header, sf_label=sf_label)
            records.append(record)
            played = len(records)
            print(f"{played}/{args.games} pair={record['paired_opening_id']} {'W' if our_white else 'B'} {game_data['our_result']} {wins}W {draws}D {losses}L", flush=True)
            if (index + 1) % 8 == 0 and index + 1 < args.games:
                restart()
    finally:
        safe_quit(our)
        safe_quit(sf)

    issues = verify_pairs(records)
    if len(records) != args.games or issues:
        raise RuntimeError(f"shard integrity failed: games={len(records)}/{args.games}; issues={issues}")
    elapsed = time.monotonic() - started
    summary = {
        "tag": args.tag,
        "time_control_seconds": base,
        "increment_seconds": increment,
        "stockfish": sf_label,
        "games": len(records),
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "score_fraction": (wins + 0.5 * draws) / len(records),
        "hash_mb_each": args.hash_mb,
        "threads_each": 1,
        "experience_book_enabled": False,
        "paired_color_opening_check": "PASS",
        "unique_opening_pairs": len({row["paired_opening_id"] for row in records}),
        "opening_offset": args.opening_offset,
        "elapsed_seconds": round(elapsed, 2),
    }
    args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("SHARD SUMMARY", json.dumps(summary, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
