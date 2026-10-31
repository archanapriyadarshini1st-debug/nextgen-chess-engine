#!/usr/bin/env python3
"""Run a resumable series of paired-color NextGen vs Stockfish 19 MAX tournaments.

Default pilot: 10 tournaments x 100 games at 3+0. Engine binaries are
fingerprinted and kept fixed for the whole data-collection series. No training
or NNUE promotion occurs inside these tournaments.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MATCH_LAB = ROOT / "tools" / "local_match_lab.py"
RECORDS_DIR = ROOT / "data" / "game_records"
TEACHER_PATH = ROOT / "data" / "teacher_positions.jsonl"
MISSED_PATH = ROOT / "data" / "missed_best_moves.jsonl"
SERIES_ROOT = ROOT / "reports" / "tournament_series"

sys.path.insert(0, str(ROOT / "tools"))
from local_match_lab import parse_time_control  # noqa: E402


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def safe_series_id(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", value or ""):
        raise ValueError("series ID must be 1-64 characters: letters, digits, dot, underscore or hyphen")
    return value


def validate_series_config(tournaments: int, games_per_tournament: int, time_control: str,
                           hash_mb: int, threads: int, teacher_every: int,
                           teacher_seconds: float, max_ply: int) -> str:
    if tournaments < 1:
        raise ValueError("tournaments must be >= 1")
    if games_per_tournament < 2 or games_per_tournament % 2:
        raise ValueError("games_per_tournament must be an even integer >= 2")
    if hash_mb < 16 or threads < 1:
        raise ValueError("hash_mb must be >= 16 and threads must be >= 1")
    if teacher_every < 1 or teacher_seconds <= 0 or max_ply < 20:
        raise ValueError("teacher interval/time must be positive and max_ply must be >= 20")
    return parse_time_control(time_control)[2]


def read_jsonl(path: Path):
    if not path.is_file():
        return
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            try:
                if line.strip():
                    yield json.loads(line)
            except json.JSONDecodeError:
                continue


def load_tournament_records(series_id: str, tournament_number: int) -> list[dict[str, Any]]:
    rows = []
    if RECORDS_DIR.is_dir():
        for path in RECORDS_DIR.glob("game-*.json"):
            try:
                row = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            if row.get("series_id") == series_id and int(row.get("tournament_number", 0) or 0) == tournament_number:
                rows.append(row)
    rows.sort(key=lambda row: int(row.get("tournament_game_number", 0) or 0))
    return rows


def pairing_issues(rows: list[dict[str, Any]]) -> list[str]:
    issues = []
    ordered = sorted(rows, key=lambda row: int(row.get("tournament_game_number", 0) or 0))
    for index in range(0, len(ordered) - 1, 2):
        first, second = ordered[index], ordered[index + 1]
        pair = index // 2 + 1
        if first.get("our_white") is not True:
            issues.append(f"pair {pair}: first game was not NextGen White")
        if second.get("our_white") is not False:
            issues.append(f"pair {pair}: second game was not NextGen Black")
        if first.get("opening", "") != second.get("opening", ""):
            issues.append(f"pair {pair}: color-reversed games used different openings")
    return issues


def _counts(values) -> dict[str, int]:
    counts: dict[str, int] = {}
    for value in values:
        key = str(value)
        counts[key] = counts.get(key, 0) + 1
    return counts


def summarize_tournament(rows: list[dict[str, Any]], target_games: int,
                         teacher_rows: list[dict] | None = None,
                         missed_rows: list[dict] | None = None) -> dict[str, Any]:
    wins = sum(row.get("our_result") == "win" for row in rows)
    draws = sum(row.get("our_result") == "draw" for row in rows)
    losses = sum(row.get("our_result") == "loss" for row in rows)
    played = len(rows)
    issues = pairing_issues(rows)
    return {
        "target_games": target_games,
        "completed_games": played,
        "remaining_games": max(0, target_games - played),
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "score_fraction": round((wins + 0.5 * draws) / played, 4) if played else None,
        "teacher_positions": len(teacher_rows or []),
        "missed_best_move_labels": len(missed_rows or []),
        "paired_color_opening_check": "PASS" if not issues else "FAIL",
        "paired_color_opening_issues": issues,
        "termination_counts": _counts(row.get("termination", "unknown") for row in rows),
        "opening_pairs_completed": sum(
            1 for i in range(0, len(rows) - 1, 2)
            if rows[i].get("our_white") is True and rows[i + 1].get("our_white") is False
            and rows[i].get("opening", "") == rows[i + 1].get("opening", "")
        ),
        "time_control": rows[0].get("time_control") if rows else None,
    }


def read_series_labels(series_id: str, tournament_number: int) -> tuple[list[dict], list[dict]]:
    teacher = [row for row in read_jsonl(TEACHER_PATH)
               if row.get("series_id") == series_id
               and int(row.get("tournament_number", 0) or 0) == tournament_number]
    missed = [row for row in read_jsonl(MISSED_PATH)
              if row.get("series_id") == series_id
              and int(row.get("tournament_number", 0) or 0) == tournament_number]
    return teacher, missed


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_engine_snapshot() -> dict[str, Any]:
    nextgen_candidates = (ROOT / "bin" / "chess_engine.exe", ROOT / "bin" / "chess_engine")
    nextgen = next((path for path in nextgen_candidates if path.is_file()), None)
    if nextgen is None:
        raise FileNotFoundError("NextGen binary missing; build bin/chess_engine.exe before starting a series.")
    stockfish_candidates = [
        ROOT / "bin" / "stockfish19.exe",
        ROOT / "bin" / "stockfish19" / "stockfish.exe",
        ROOT / "bin" / "stockfish19" / "stockfish-windows-x86-64.exe",
        ROOT / "bin" / "stockfish19" / "stockfish-windows-x86-64-avx2.exe",
    ]
    stockfish_dir = ROOT / "bin" / "stockfish19"
    if stockfish_dir.is_dir():
        stockfish_candidates.extend(sorted(stockfish_dir.rglob("*.exe")))
    stockfish = next((path for path in stockfish_candidates if path.is_file()), None)
    if stockfish is None:
        raise FileNotFoundError("Stockfish 19 executable missing under bin/stockfish19/.")
    return {
        "nextgen": {"path": str(nextgen.relative_to(ROOT)), "sha256": sha256_file(nextgen),
                    "size_bytes": nextgen.stat().st_size},
        "stockfish": {"path": str(stockfish.relative_to(ROOT)), "sha256": sha256_file(stockfish),
                      "size_bytes": stockfish.stat().st_size},
    }


def atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temp.replace(path)


def count_all_series_games(series_id: str) -> int:
    count = 0
    if RECORDS_DIR.is_dir():
        for path in RECORDS_DIR.glob("game-*.json"):
            try:
                row = json.loads(path.read_text(encoding="utf-8"))
                count += int(row.get("series_id") == series_id)
            except Exception:
                continue
    return count


def build_series_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    totals = {"wins": 0, "draws": 0, "losses": 0, "games": 0}
    tournament_rows = []
    config = manifest["config"]
    for number in range(1, int(config["tournaments"]) + 1):
        entry = manifest.get("tournaments", {}).get(f"{number:02d}", {})
        summary = entry.get("summary", {})
        for field in totals:
            totals[field] += int(summary.get("completed_games" if field == "games" else field, 0))
        tournament_rows.append({"tournament": number, **summary, "status": entry.get("status", "pending")})
    games = totals["games"]
    target = int(config["tournaments"]) * int(config["games_per_tournament"])
    return {
        "schema": "nextgen-tournament-series-summary-v1",
        "series_id": manifest["series_id"],
        "status": manifest["status"],
        "config": config,
        "engine_snapshot": manifest["engine_snapshot"],
        "totals": {
            **totals,
            "target_games": target,
            "remaining_games": max(0, target - games),
            "score_fraction": round((totals["wins"] + 0.5 * totals["draws"]) / games, 4) if games else None,
        },
        "tournaments": tournament_rows,
        "updated_utc": utc_now(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--series-id", default=None, help="series ID printed on the first run; reuse to resume")
    parser.add_argument("--tournaments", type=int, default=10)
    parser.add_argument("--games-per-tournament", type=int, default=100)
    parser.add_argument("--time-control", default="3+0")
    parser.add_argument("--hash-mb", type=int, default=64)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--teacher-every", type=int, default=4)
    parser.add_argument("--teacher-seconds", type=float, default=3.0)
    parser.add_argument("--min-cp-loss", type=int, default=50)
    parser.add_argument("--max-ply", type=int, default=600)
    parser.add_argument("--pause-between-games", type=float, default=1.0)
    parser.add_argument("--pause-between-tournaments", type=float, default=2.0)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()

    try:
        tc = validate_series_config(args.tournaments, args.games_per_tournament, args.time_control,
                                    args.hash_mb, args.threads, args.teacher_every,
                                    args.teacher_seconds, args.max_ply)
        series_id = safe_series_id(args.series_id) if args.series_id else None
    except ValueError as exc:
        parser.error(str(exc))
    if args.min_cp_loss < 0 or args.pause_between_games < 0 or args.pause_between_tournaments < 0:
        parser.error("minimum cp loss and pause intervals must be nonnegative")
    target_games = args.tournaments * args.games_per_tournament

    if args.validate_only:
        print(json.dumps({
            "status": "VALID", "series_id": series_id or "<generated-on-run>",
            "tournaments": args.tournaments, "games_per_tournament": args.games_per_tournament,
            "total_games": target_games, "time_control": tc, "paired_colors": True,
            "teacher_every_nextgen_moves": args.teacher_every,
            "teacher_seconds_per_analysis": args.teacher_seconds,
            "engine_updates_during_series": False, "resume_supported": True,
        }, indent=2))
        return 0

    if sys.platform != "win32":
        raise SystemExit("The local tournament series runs on the Windows Match Lab PC.")
    if not MATCH_LAB.is_file():
        raise SystemExit(f"Local Match Lab runner missing: {MATCH_LAB}")
    if series_id is None:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        series_id = f"sf19-max-{tc.replace('+', 'p')}-{stamp}"
    safe_series_id(series_id)

    output_dir = SERIES_ROOT / series_id
    manifest_path = output_dir / "series_manifest.json"
    summary_path = output_dir / "series_summary.json"
    config = {
        "tournaments": args.tournaments, "games_per_tournament": args.games_per_tournament,
        "total_games": target_games, "time_control": tc, "hash_mb_each": args.hash_mb,
        "threads_each": args.threads, "teacher_every_nth_nextgen_move": args.teacher_every,
        "teacher_seconds_per_analysis": args.teacher_seconds, "minimum_cp_loss": args.min_cp_loss,
        "max_ply": args.max_ply,
    }

    snapshot = find_engine_snapshot()
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise SystemExit(f"Cannot read saved series manifest: {exc}")
        if manifest.get("config") != config:
            raise SystemExit("Series settings differ from saved manifest. Resume with the original settings.")
        if manifest.get("status") != "complete" and manifest.get("engine_snapshot") != snapshot:
            raise SystemExit("Engine binary changed since this series began. Start a new series ID.")
    else:
        manifest = {
            "schema": "nextgen-tournament-series-v1", "series_id": series_id,
            "status": "pending", "config": config, "engine_snapshot": snapshot,
            "created_utc": utc_now(), "tournaments": {},
        }
        atomic_json(manifest_path, manifest)

    print(f"NextGen tournament series: {series_id}", flush=True)
    print(f"Plan: {args.tournaments} x {args.games_per_tournament} = {target_games} games at {tc}", flush=True)
    print("Engine hashes are pinned; no NNUE training or promotion happens during the series.", flush=True)
    manifest["status"] = "running"
    atomic_json(manifest_path, manifest)

    try:
        for number in range(1, args.tournaments + 1):
            key = f"{number:02d}"
            rows = load_tournament_records(series_id, number)
            saved = len(rows)
            if saved > args.games_per_tournament:
                raise SystemExit(f"Tournament {number} already has more than the configured target.")
            if saved == args.games_per_tournament:
                teacher, missed = read_series_labels(series_id, number)
                summary = summarize_tournament(rows, args.games_per_tournament, teacher, missed)
                manifest["tournaments"][key] = {"status": "complete", "summary": summary, "updated_utc": utc_now()}
                atomic_json(manifest_path, manifest)
                print(f"Tournament {number}/{args.tournaments}: already complete.", flush=True)
                continue

            remaining = args.games_per_tournament - saved
            manifest["tournaments"][key] = {
                "status": "running",
                "summary": summarize_tournament(rows, args.games_per_tournament),
                "updated_utc": utc_now(),
            }
            atomic_json(manifest_path, manifest)
            print(f"\n=== TOURNAMENT {number}/{args.tournaments}: {saved}/{args.games_per_tournament} saved; {remaining} remaining ===", flush=True)

            command = [
                sys.executable, str(MATCH_LAB), "--time-control", tc,
                "--hash-mb", str(args.hash_mb), "--threads", str(args.threads),
                "--teacher-every", str(args.teacher_every), "--teacher-seconds", str(args.teacher_seconds),
                "--min-cp-loss", str(args.min_cp_loss), "--max-ply", str(args.max_ply),
                "--pause-between-games", str(args.pause_between_games), "--max-games", str(remaining),
                "--series-id", series_id, "--tournament-number", str(number),
                "--tournament-games", str(args.games_per_tournament),
            ]
            child = subprocess.run(command, cwd=ROOT, check=False)
            current_rows = load_tournament_records(series_id, number)
            teacher, missed = read_series_labels(series_id, number)
            summary = summarize_tournament(current_rows, args.games_per_tournament, teacher, missed)
            manifest["tournaments"][key] = {
                "status": "complete" if len(current_rows) == args.games_per_tournament else "paused",
                "summary": summary, "runner_exit_code": child.returncode, "updated_utc": utc_now(),
            }
            atomic_json(manifest_path, manifest)

            if child.returncode not in (0, 3):
                manifest["status"] = "failed"
                atomic_json(manifest_path, manifest)
                raise SystemExit(f"Tournament {number} runner exited with code {child.returncode}.")
            if len(current_rows) < args.games_per_tournament:
                manifest["status"] = "paused"
                atomic_json(manifest_path, manifest)
                print(f"Series paused safely during tournament {number}. Resume using the same series ID.", flush=True)
                break
            if summary["paired_color_opening_check"] != "PASS":
                manifest["status"] = "failed"
                atomic_json(manifest_path, manifest)
                raise SystemExit(f"Tournament {number} failed paired-color/opening validation.")
            if find_engine_snapshot() != manifest["engine_snapshot"]:
                manifest["status"] = "failed"
                atomic_json(manifest_path, manifest)
                raise SystemExit("Engine binary changed during the series; refusing to continue.")
            print(f"Tournament {number} complete: {summary['wins']}W {summary['draws']}D {summary['losses']}L; labels={summary['teacher_positions']}.", flush=True)
            if number < args.tournaments:
                time.sleep(args.pause_between_tournaments)
    except KeyboardInterrupt:
        manifest["status"] = "paused"
        atomic_json(manifest_path, manifest)
        print("\nSeries interrupted. Completed games remain saved; resume using the same series ID.", flush=True)

    all_complete = all(
        len(load_tournament_records(series_id, number)) == args.games_per_tournament
        for number in range(1, args.tournaments + 1)
    )
    manifest["status"] = "complete" if all_complete else "paused"
    manifest["updated_utc"] = utc_now()
    manifest["total_completed_games"] = count_all_series_games(series_id)
    atomic_json(manifest_path, manifest)
    summary = build_series_summary(manifest)
    atomic_json(summary_path, summary)
    print("\nSERIES SUMMARY", json.dumps(summary["totals"], indent=2), flush=True)
    print(f"Manifest: {manifest_path}", flush=True)
    print(f"Summary: {summary_path}", flush=True)
    return 0 if all_complete else 3


if __name__ == "__main__":
    raise SystemExit(main())
