#!/usr/bin/env python3
"""Persistent local NextGen vs Stockfish 19 training match runner.

Completed games and positions are flushed to disk. Restarting the program
continues from the number of saved games; an interrupted game is discarded.
"""
from __future__ import annotations

import argparse
import json
import os
try:
    import msvcrt
except ImportError:  # Keep time-control parsing testable on non-Windows CI.
    msvcrt = None
import re
import sys
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import chess
import chess.engine
import chess.pgn

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "bin"
DATA = ROOT / "data"
REPORTS = ROOT / "reports"
OUR = BIN / "chess_engine.exe"
STOP = ROOT / "STOP_AFTER_GAME"
GAMES_JSONL = DATA / "games.jsonl"
POSITIONS_JSONL = DATA / "positions.jsonl"
TEACHER_JSONL = DATA / "teacher_positions.jsonl"
MISSED_JSONL = DATA / "missed_best_moves.jsonl"
LEGACY_TEACHER_JSONL = DATA / "legacy" / "teacher_positions_fast_350ms_legacy.jsonl"
PGN_PATH = DATA / "games.pgn"
STATE_PATH = REPORTS / "session_state.json"
SUMMARY_PATH = REPORTS / "session_summary.json"
GAME_RECORDS_DIR = DATA / "game_records"
LOCK_PATH = ROOT / "match_lab.lock"
LOCK_HANDLE = None

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
]

def now() -> str:
    return datetime.now(timezone.utc).isoformat()

def parse_time_control(value: str) -> tuple[float, float, str]:
    """Parse chess clock notation: base minutes + increment seconds (e.g. 3+2)."""
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*\+\s*(\d+(?:\.\d+)?)\s*", value)
    if not match:
        raise ValueError("Use Chess.com-style notation: base minutes + increment seconds, e.g. 3+0 or 5+3.")
    minutes = float(match.group(1))
    increment = float(match.group(2))
    if not (0 < minutes <= 1440) or not (0 <= increment <= 3600):
        raise ValueError("Base time must be > 0 and <= 1440 minutes; increment must be between 0 and 3600 seconds.")
    return minutes * 60.0, increment, f"{minutes:g}+{increment:g}"

def acquire_process_lock() -> None:
    global LOCK_HANDLE
    if msvcrt is None:
        raise RuntimeError("The local Match Lab process lock requires Windows.")
    LOCK_HANDLE = LOCK_PATH.open("a+b")
    LOCK_HANDLE.seek(0, os.SEEK_END)
    if LOCK_HANDLE.tell() == 0:
        LOCK_HANDLE.write(b"0")
        LOCK_HANDLE.flush()
    LOCK_HANDLE.seek(0)
    try:
        msvcrt.locking(LOCK_HANDLE.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError as exc:
        LOCK_HANDLE.close()
        LOCK_HANDLE = None
        raise RuntimeError("Another Match Lab session is already running.") from exc

def release_process_lock() -> None:
    global LOCK_HANDLE
    if LOCK_HANDLE is not None:
        try:
            LOCK_HANDLE.seek(0)
            msvcrt.locking(LOCK_HANDLE.fileno(), msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        LOCK_HANDLE.close()
        LOCK_HANDLE = None

def append_jsonl(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        stream.flush()
        os.fsync(stream.fileno())

def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)

def read_jsonl(path: Path):
    if not path.exists():
        return
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            try:
                if line.strip():
                    yield json.loads(line)
            except json.JSONDecodeError:
                continue

def read_count(path: Path) -> int:
    return sum(1 for _ in read_jsonl(path)) if path.exists() else 0

def completed_games() -> int:
    if not GAME_RECORDS_DIR.is_dir():
        return 0
    count = 0
    for path in GAME_RECORDS_DIR.glob("game-*.json"):
        try:
            row = json.loads(path.read_text(encoding="utf-8"))
            if row.get("game_id"):
                count += 1
        except Exception:
            continue
    return count

def find_stockfish() -> Path:
    candidates = [
        BIN / "stockfish19" / "stockfish.exe",
        BIN / "stockfish19" / "stockfish-windows-x86-64.exe",
        BIN / "stockfish19" / "stockfish-windows-x86-64-avx2.exe",
        BIN / "stockfish19.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    matches = sorted((BIN / "stockfish19").rglob("*.exe")) if (BIN / "stockfish19").is_dir() else []
    if matches:
        return matches[0]
    raise FileNotFoundError("Stockfish 19 executable missing from bin\\stockfish19.")

def start_engine(path: Path, hash_mb: int, threads: int, is_stockfish: bool):
    engine = chess.engine.SimpleEngine.popen_uci(str(path), timeout=15.0)
    options = {}
    if "Hash" in engine.options:
        options["Hash"] = hash_mb
    if "Threads" in engine.options:
        options["Threads"] = threads
    if not is_stockfish:
        if "OwnBook" in engine.options:
            options["OwnBook"] = False
        if "NNUEFile" in engine.options:
            # Keep the first baseline clean; only opt into a tested candidate deliberately.
            pass
    else:
        if "UCI_LimitStrength" in engine.options:
            options["UCI_LimitStrength"] = False
    if options:
        engine.configure(options)
    return engine

def safe_quit(engine) -> None:
    if engine is None:
        return
    try:
        engine.quit()
    except Exception:
        try:
            engine.protocol.close()
        except Exception:
            pass

def make_game(opening: str):
    board = chess.Board()
    game = chess.pgn.Game()
    node = game
    if opening:
        for token in opening.split():
            move = chess.Move.from_uci(token)
            if move not in board.legal_moves:
                raise ValueError(f"Invalid configured opening move: {token}")
            node = node.add_variation(move)
            board.push(move)
    return board, game, node

def phase(board: chess.Board) -> str:
    if board.ply() < 20:
        return "opening"
    if len(board.piece_map()) <= 10 or board.ply() >= 80:
        return "endgame"
    return "middlegame"

def play_one(our, sf, game_number: int, our_white: bool, opening: str,
             clock_seconds: float, increment_seconds: float, max_ply: int):
    board, game, node = make_game(opening)
    our_color = chess.WHITE if our_white else chess.BLACK
    clocks = {chess.WHITE: clock_seconds, chess.BLACK: clock_seconds}
    our_positions = []
    moves = []
    move_times = []
    termination = "unknown"
    crash = None
    flag_fell = None
    while not board.is_game_over(claim_draw=True) and board.ply() < max_ply:
        is_our = board.turn == our_color
        engine = our if is_our else sf
        fen_before = board.fen()
        side_to_move = board.turn
        clock_before = clocks[side_to_move]
        started = time.monotonic()
        try:
            limit = chess.engine.Limit(
                white_clock=max(0.001, clocks[chess.WHITE]),
                black_clock=max(0.001, clocks[chess.BLACK]),
                white_inc=increment_seconds,
                black_inc=increment_seconds,
            )
            result = engine.play(board, limit)
        except Exception as exc:
            crash = f"{'NextGen' if is_our else 'Stockfish 19'} engine error: {exc}"
            break
        elapsed = time.monotonic() - started
        move = result.move
        if elapsed >= clock_before:
            clocks[side_to_move] = 0.0
            flag_fell = side_to_move
            termination = "time_forfeit"
            move_times.append({
                "ply": board.ply(),
                "color": "white" if side_to_move else "black",
                "engine": "NextGen" if is_our else "Stockfish 19",
                "elapsed_seconds": round(elapsed, 4),
                "clock_before_seconds": round(clock_before, 4),
                "clock_after_seconds": 0.0,
            })
            break
        clocks[side_to_move] = clock_before - elapsed + increment_seconds
        move_times.append({
            "ply": board.ply(),
            "color": "white" if side_to_move else "black",
            "engine": "NextGen" if is_our else "Stockfish 19",
            "elapsed_seconds": round(elapsed, 4),
            "clock_before_seconds": round(clock_before, 4),
            "clock_after_seconds": round(clocks[side_to_move], 4),
        })
        if move is None or move not in board.legal_moves:
            crash = f"{'NextGen' if is_our else 'Stockfish 19'} returned an illegal or null move."
            break
        if is_our:
            our_positions.append({
                "game_id": f"game-{game_number:08d}",
                "move_index": len(our_positions) + 1,
                "ply": board.ply(),
                "fen": fen_before,
                "played": move.uci(),
                "phase": phase(board),
                "our_color": "white" if our_white else "black",
                "think_seconds": round(elapsed, 4),
                "clock_remaining_seconds": round(clocks[side_to_move], 4),
            })
        node = node.add_variation(move)
        board.push(move)
        moves.append(move.uci())

    if crash:
        termination = "discarded_engine_error"
        return None, crash
    if flag_fell is not None:
        winner = not flag_fell
        if board.has_insufficient_material(winner):
            result_text = "1/2-1/2"
        else:
            result_text = "1-0" if winner == chess.WHITE else "0-1"
    elif board.is_game_over(claim_draw=True):
        outcome = board.outcome(claim_draw=True)
        result_text = board.result(claim_draw=True)
        termination = outcome.termination.name if outcome else "game_over"
    else:
        result_text = "1/2-1/2"
        termination = "max_ply"
    if result_text == "1-0":
        our_result = "win" if our_white else "loss"
    elif result_text == "0-1":
        our_result = "loss" if our_white else "win"
    else:
        our_result = "draw"

    game.headers["Event"] = "NextGen Local Match Lab"
    game.headers["Site"] = "Local Windows PC"
    game.headers["Date"] = datetime.now().strftime("%Y.%m.%d")
    game.headers["Round"] = str(game_number)
    game.headers["White"] = "NextGen" if our_white else "Stockfish 19"
    game.headers["Black"] = "Stockfish 19" if our_white else "NextGen"
    game.headers["Result"] = result_text
    game.headers["TimeControl"] = f"{clock_seconds:g}+{increment_seconds:g}"
    game.headers["Termination"] = termination
    game.headers["OpeningUCI"] = opening
    game.headers["PlyCount"] = str(board.ply())

    return {
        "game": game,
        "game_id": f"game-{game_number:08d}",
        "timestamp_utc": now(),
        "game_number": game_number,
        "our_white": our_white,
        "our_result": our_result,
        "result": result_text,
        "termination": termination,
        "opening": opening,
        "ply": board.ply(),
        "moves": moves,
        "move_times": move_times,
        "clock_final_seconds": {
            "white": round(clocks[chess.WHITE], 4),
            "black": round(clocks[chess.BLACK], 4),
        },
        "final_fen": board.fen(),
        "positions": our_positions,
    }, None

def append_pgn(game: chess.pgn.Game) -> None:
    PGN_PATH.parent.mkdir(parents=True, exist_ok=True)
    with PGN_PATH.open("a", encoding="utf-8") as stream:
        print(game, file=stream, end="\n\n")
        stream.flush()
        os.fsync(stream.fileno())

def game_from_record(row: dict) -> chess.pgn.Game:
    board, game, node = make_game(row.get("opening", ""))
    for token in row.get("moves", []):
        move = chess.Move.from_uci(token)
        if move not in board.legal_moves:
            raise ValueError(f"Saved game {row.get('game_id')} contains illegal move {token}")
        node = node.add_variation(move)
        board.push(move)
    game.headers["Event"] = "NextGen Local Match Lab"
    game.headers["Site"] = "Local Windows PC"
    game.headers["Date"] = str(row.get("date", datetime.now().strftime("%Y.%m.%d")))
    game.headers["Round"] = str(row.get("game_number", ""))
    game.headers["White"] = "NextGen" if row.get("our_white") else "Stockfish 19"
    game.headers["Black"] = "Stockfish 19" if row.get("our_white") else "NextGen"
    game.headers["Result"] = row.get("result", "*")
    # PGN TimeControl is expressed in seconds. Keep the Chess.com-style
    # minutes+increment label in a separate game-record field.
    game.headers["TimeControl"] = str(row.get("time_control_pgn", row.get("time_control", "")))
    game.headers["Termination"] = str(row.get("termination", ""))
    game.headers["OpeningUCI"] = str(row.get("opening", ""))
    game.headers["PlyCount"] = str(row.get("ply", ""))
    return game

def refresh_exports_from_records() -> None:
    """Rebuild convenience exports from atomic per-game records after every restart."""
    GAME_RECORDS_DIR.mkdir(parents=True, exist_ok=True)
    rows = []
    for path in sorted(GAME_RECORDS_DIR.glob("game-*.json")):
        try:
            row = json.loads(path.read_text(encoding="utf-8"))
            if row.get("game_id"):
                rows.append(row)
        except Exception as exc:
            print(f"Warning: skipped unreadable game record {path.name}: {exc}", flush=True)

    games_tmp = GAMES_JSONL.with_suffix(".jsonl.tmp")
    positions_tmp = POSITIONS_JSONL.with_suffix(".jsonl.tmp")
    pgn_tmp = PGN_PATH.with_suffix(".pgn.tmp")
    with (
        games_tmp.open("w", encoding="utf-8") as games_out,
        positions_tmp.open("w", encoding="utf-8") as positions_out,
        pgn_tmp.open("w", encoding="utf-8") as pgn_out,
    ):
        for row in rows:
            summary = {k: v for k, v in row.items() if k != "positions"}
            summary["positions_logged"] = len(row.get("positions", []))
            games_out.write(json.dumps(summary, ensure_ascii=False) + "\n")
            for position in row.get("positions", []):
                derived = dict(position)
                derived["time_control"] = derived.get("time_control", row.get("time_control", "unknown"))
                derived["our_result"] = row.get("our_result")
                derived["result"] = row.get("result")
                positions_out.write(json.dumps(derived, ensure_ascii=False) + "\n")
            try:
                print(game_from_record(row), file=pgn_out, end="\n\n")
            except Exception as exc:
                print(f"Warning: could not rebuild PGN for {row.get('game_id')}: {exc}", flush=True)
        for stream in (games_out, positions_out, pgn_out):
            stream.flush()
            os.fsync(stream.fileno())
    os.replace(games_tmp, GAMES_JSONL)
    os.replace(positions_tmp, POSITIONS_JSONL)
    os.replace(pgn_tmp, PGN_PATH)

def teacher_done_keys() -> set[str]:
    # Legacy fast labels are archived and excluded from current training, but
    # their IDs remain marked complete so startup does not silently relabel
    # hundreds of historic fast-game positions.
    done = set()
    for source in (TEACHER_JSONL, LEGACY_TEACHER_JSONL):
        for row in read_jsonl(source):
            done.add(str(row.get("position_id", "")))
    return done

def label_pending_positions(sf, every_n: int, teacher_seconds: float,
                            min_cp_loss: int, state: dict,
                            position_rows=None, done=None) -> int:
    # At startup, scan the dataset once to recover any interrupted teacher labels.
    # During normal operation, analyse only the just-finished game's positions.
    if done is None:
        done = teacher_done_keys()
    source_rows = read_jsonl(POSITIONS_JSONL) if position_rows is None else position_rows
    added = 0
    missed_added = 0
    for row in source_rows:
        index = int(row.get("move_index", 0))
        if index == 0 or (index - 1) % every_n != 0:
            continue
        position_id = f"{row['game_id']}:{index}"
        if position_id in done:
            continue
        try:
            board = chess.Board(row["fen"])
            played = chess.Move.from_uci(row["played"])
            if played not in board.legal_moves:
                continue
            limit = chess.engine.Limit(time=teacher_seconds)
            info = sf.analyse(board, limit)
            pv = info.get("pv") or []
            if not pv:
                continue
            best = pv[0]
            best_cp = info["score"].pov(board.turn).score(mate_score=10000)
            if best_cp is None:
                continue
            played_info = sf.analyse(board, limit, root_moves=[played])
            played_cp = played_info["score"].pov(board.turn).score(mate_score=10000)
            if played_cp is None:
                continue
            cp_loss = max(0, int(best_cp - played_cp))
            teacher_row = {
                "position_id": position_id,
                "game_id": row["game_id"],
                "fen": row["fen"],
                "best_cp": int(best_cp),
                "best": best.uci(),
                "teacher_depth": int(info.get("depth") or 1),
                "teacher_time_seconds": teacher_seconds,
                "teacher_time_scope": "per_search_best_move_and_played_move_scoring",
                "requested_depth": None,
                "label_source": "stockfish19_local_match_lab",
                "played": played.uci(),
                "played_cp": int(played_cp),
                "cp_loss": cp_loss,
                "phase": row.get("phase"),
                "time_control": row.get("time_control", "unknown"),
                "our_result": row.get("our_result"),
                "timestamp_utc": now(),
            }
            append_jsonl(TEACHER_JSONL, teacher_row)
            if cp_loss >= min_cp_loss:
                missed = dict(teacher_row)
                missed["severity"] = "blunder" if cp_loss >= 300 else "mistake" if cp_loss >= 100 else "inaccuracy"
                append_jsonl(MISSED_JSONL, missed)
                missed_added += 1
            done.add(position_id)
            added += 1
        except (chess.InvalidMoveError, ValueError, KeyError, chess.engine.EngineError) as exc:
            print(f"  teacher analysis skipped {position_id}: {exc}", flush=True)
        except Exception as exc:
            print(f"  teacher analysis warning {position_id}: {exc}", flush=True)
    state["teacher_positions"] = int(state.get("teacher_positions", 0)) + added
    state["missed_moves"] = int(state.get("missed_moves", 0)) + missed_added
    return added

def results_by_time_control() -> dict:
    grouped = {}
    for row in read_jsonl(GAMES_JSONL):
        control = str(row.get("time_control", "unknown"))
        result = str(row.get("our_result", "unknown"))
        grouped.setdefault(control, Counter())[result] += 1
    return {control: dict(counts) for control, counts in grouped.items()}

def write_summary(state: dict, results: Counter) -> None:
    state["results"] = dict(results)
    state["results_by_time_control"] = results_by_time_control()
    state["updated_utc"] = now()
    atomic_json(STATE_PATH, state)
    atomic_json(SUMMARY_PATH, state)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--time-control", default="3+0",
                        help="Chess clock notation: base minutes + increment seconds, e.g. 1+0, 3+0, 5+3, 10+5")
    parser.add_argument("--clock-seconds", type=float, default=None,
                        help="legacy override: starting clock seconds per side")
    parser.add_argument("--increment-seconds", type=float, default=None,
                        help="legacy override: Fischer increment seconds per move")
    parser.add_argument("--hash-mb", type=int, default=64,
                        help="Hash memory per engine; 64 MB each is a conservative starting point")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--teacher-every", type=int, default=4,
                        help="teacher-label every Nth NextGen move; default 4")
    parser.add_argument("--teacher-seconds", type=float, default=3.0,
                        help="Stockfish analysis budget for each best-move and played-move evaluation")
    parser.add_argument("--min-cp-loss", type=int, default=50)
    parser.add_argument("--max-ply", type=int, default=600,
                        help="safety cap; 600 plies is deliberately high for a 3+0 game")
    parser.add_argument("--pause-between-games", type=float, default=1.0)
    parser.add_argument("--max-games", type=int, default=0,
                        help="0 means continuous until Stop-After-Game is clicked")
    parser.add_argument("--validate-only", action="store_true",
                        help="validate and print the selected clock without starting engines")
    args = parser.parse_args()

    try:
        parsed_clock, parsed_increment, time_control_label = parse_time_control(args.time_control)
    except ValueError as exc:
        parser.error(str(exc))
    args.clock_seconds = parsed_clock if args.clock_seconds is None else args.clock_seconds
    args.increment_seconds = parsed_increment if args.increment_seconds is None else args.increment_seconds
    if args.clock_seconds <= 0 or args.increment_seconds < 0 or args.hash_mb < 16 or args.threads < 1:
        parser.error("clock must be > 0; increment must be >= 0; Hash must be >= 16 MB; Threads must be >= 1")
    if args.clock_seconds != parsed_clock or args.increment_seconds != parsed_increment:
        time_control_label = f"{args.clock_seconds / 60:g}+{args.increment_seconds:g}"
    if args.teacher_every < 1 or args.teacher_seconds <= 0 or args.max_ply < 20:
        parser.error("teacher interval/time must be positive and max-ply must be >= 20")
    if args.validate_only:
        print(f"Valid time control: {time_control_label} ({args.clock_seconds:g} seconds base + {args.increment_seconds:g} seconds increment)")
        return 0
    if not OUR.is_file():
        raise SystemExit(f"NextGen executable missing: {OUR}")
    sf_path = find_stockfish()
    for folder in (DATA, REPORTS, ROOT / "candidates", GAME_RECORDS_DIR):
        folder.mkdir(parents=True, exist_ok=True)
    acquire_process_lock()
    # Remove a stale stop request only after owning the single-instance lock.
    # A second Start click therefore cannot cancel another session's stop request.
    STOP.unlink(missing_ok=True)

    state = {}
    if STATE_PATH.exists():
        try:
            state = json.loads(STATE_PATH.read_text(encoding="utf-8"))
        except Exception:
            state = {}
    GAME_RECORDS_DIR.mkdir(parents=True, exist_ok=True)
    refresh_exports_from_records()
    completed = completed_games()
    positions_total = read_count(POSITIONS_JSONL)
    state["teacher_positions"] = read_count(TEACHER_JSONL)
    state["missed_moves"] = read_count(MISSED_JSONL)
    results = Counter(row.get("our_result", "unknown") for row in read_jsonl(GAMES_JSONL))
    teacher_keys = teacher_done_keys()
    state.update({
        "schema": "nextgen-local-match-lab-v1",
        "next_game_number": completed + 1,
        "completed_games": completed,
        "discarded_games": int(state.get("discarded_games", 0)),
        "clock_seconds_each": args.clock_seconds,
        "increment_seconds": args.increment_seconds,
        "time_control": time_control_label,
        "hash_mb_each": args.hash_mb,
        "threads_each": args.threads,
        "stockfish": "Stockfish 19 MAX (UCI_LimitStrength=false)",
        "nextgen_experience_book": False,
        "teacher_every_nth_nextgen_move": args.teacher_every,
        "teacher_seconds_per_analysis": args.teacher_seconds,
    })
    print("=" * 64)
    print("NEXTGEN LOCAL MATCH LAB")
    print(f"NextGen: {OUR}")
    print(f"Stockfish: {sf_path}")
    print(f"Completed games saved: {completed}")
    print(f"Resources: {args.threads} thread(s) and {args.hash_mb} MB Hash per engine")
    print(f"Clock: {time_control_label} (real-time UCI clock; base is minutes, increment is seconds)")
    print(f"Teacher: {args.teacher_seconds:g}s per Stockfish analysis, every {args.teacher_every}th NextGen move")
    print("Stockfish: full strength; NextGen experience book: OFF")
    print("Press Stop-After-Game.bat to finish the current game and stop.")
    print("=" * 64, flush=True)

    our = sf = None
    started = time.monotonic()
    wins = losses = draws = discarded = 0
    try:
        our = start_engine(OUR, args.hash_mb, args.threads, False)
        sf = start_engine(sf_path, args.hash_mb, args.threads, True)
        # Finish any teacher analysis interrupted by a prior shutdown.
        pending = label_pending_positions(sf, args.teacher_every, args.teacher_seconds,
                                          args.min_cp_loss, state, done=teacher_keys)
        if pending:
            print(f"Recovered {pending} pending teacher labels.", flush=True)

        new_games = 0
        while True:
            if STOP.exists():
                print("Stop request found. No new game will be started.", flush=True)
                break
            if args.max_games and new_games >= args.max_games:
                break
            game_number = completed + 1
            pair_number = (game_number - 1) // 2
            our_white = (game_number % 2 == 1)
            opening = OPENINGS[pair_number % len(OPENINGS)]
            print(f"\nGame {game_number}: NextGen {'White' if our_white else 'Black'} | opening {opening or 'start position'}", flush=True)
            match, error = play_one(our, sf, game_number, our_white, opening,
                                    args.clock_seconds, args.increment_seconds, args.max_ply)
            if error:
                discarded += 1
                state["discarded_games"] = int(state.get("discarded_games", 0)) + 1
                print("  Discarded incomplete game: " + error, flush=True)
                safe_quit(our)
                safe_quit(sf)
                time.sleep(1)
                our = start_engine(OUR, args.hash_mb, args.threads, False)
                sf = start_engine(sf_path, args.hash_mb, args.threads, True)
                if discarded >= 3:
                    raise RuntimeError("Three consecutive engine failures; stopping for safety.")
                continue

            game = match.pop("game")
            record = dict(match)
            record["date"] = datetime.now().strftime("%Y.%m.%d")
            record["time_control"] = time_control_label
            record["time_control_pgn"] = f"{args.clock_seconds:g}+{args.increment_seconds:g}"
            for position in record["positions"]:
                position["time_control"] = time_control_label
            # One atomic JSON file is the source of truth for each completed game.
            # The combined exports are appended only after the canonical record exists.
            # If power is lost mid-export, startup rebuilds them from the per-game records.
            atomic_json(GAME_RECORDS_DIR / f"{record['game_id']}.json", record)
            append_pgn(game)
            derived_positions = []
            for position in record["positions"]:
                derived = dict(position)
                derived["time_control"] = time_control_label
                derived["our_result"] = record["our_result"]
                derived["result"] = record["result"]
                append_jsonl(POSITIONS_JSONL, derived)
                derived_positions.append(derived)
            summary_record = {k: v for k, v in record.items() if k != "positions"}
            summary_record["positions_logged"] = len(derived_positions)
            append_jsonl(GAMES_JSONL, summary_record)

            completed += 1
            positions_total += len(derived_positions)
            results[match["our_result"]] += 1
            new_games += 1
            if match["our_result"] == "win":
                wins += 1
            elif match["our_result"] == "loss":
                losses += 1
            else:
                draws += 1
            print(f"  Result: {match['our_result']} | {match['result']} | {match['ply']} plies", flush=True)
            print(f"  Session score: {wins}W {draws}D {losses}L | positions: {positions_total}", flush=True)

            labelled = label_pending_positions(
                sf, args.teacher_every, args.teacher_seconds, args.min_cp_loss,
                state, position_rows=derived_positions, done=teacher_keys
            )
            print(f"  New teacher labels: {labelled}", flush=True)

            state.update({
                "completed_games": completed,
                "next_game_number": completed + 1,
                "session_games": new_games,
                "session_wins": wins,
                "session_draws": draws,
                "session_losses": losses,
                "session_discarded": discarded,
                "positions": positions_total,
                "teacher_positions": state.get("teacher_positions", 0),
                "missed_moves": state.get("missed_moves", 0),
                "clock_seconds_each": args.clock_seconds,
                "increment_seconds": args.increment_seconds,
                "time_control": time_control_label,
                "teacher_seconds_per_analysis": args.teacher_seconds,
                "elapsed_seconds_this_session": round(time.monotonic() - started, 1),
                "last_result": match["our_result"],
            })
            write_summary(state, results)
            if STOP.exists():
                print("Stop request received. Current game and labels are saved.", flush=True)
                break
            time.sleep(max(0.0, args.pause_between_games))
    except KeyboardInterrupt:
        print("\nInterrupted. Completed games remain saved; an unfinished game is discarded.", flush=True)
    finally:
        safe_quit(our)
        safe_quit(sf)
        state.update({
            "completed_games": completed,
            "positions": positions_total,
            "teacher_positions": state.get("teacher_positions", 0),
            "missed_moves": state.get("missed_moves", 0),
            "elapsed_seconds_this_session": round(time.monotonic() - started, 1),
            "stopped_utc": now(),
        })
        try:
            STOP.unlink(missing_ok=True)
            write_summary(state, results)
        finally:
            release_process_lock()
    print("\nMatch Lab stopped. All completed games and teacher labels are saved.", flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
