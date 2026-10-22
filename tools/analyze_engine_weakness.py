#!/usr/bin/env python3
"""Find and deeply audit NextGen's mistakes from a PGN match.

The first pass asks Stockfish to choose its own depth under a small time budget
(no fixed depth). The largest losses are then re-analysed with a much larger
time budget, again without a forced depth. This is diagnostic analysis, not an
absolute Elo measurement or a claim that Stockfish's first line is infallible.
"""
from __future__ import annotations

import argparse
import json
import statistics
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import chess
import chess.engine
import chess.pgn

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STOCKFISH = Path("/tmp/sf19/stockfish19")


def phase_of(board: chess.Board) -> str:
    material = sum(
        {chess.PAWN: 1, chess.KNIGHT: 3, chess.BISHOP: 3, chess.ROOK: 5, chess.QUEEN: 9}.get(
            piece.piece_type, 0
        )
        for piece in board.piece_map().values()
    )
    if board.fullmove_number <= 10:
        return "opening"
    if material <= 16 or board.fullmove_number >= 45:
        return "endgame"
    return "middlegame"


def collect_nextgen_positions(pgn_path: Path) -> list[dict[str, Any]]:
    """Extract each NextGen move with its exact pre-move FEN from all PGN mainlines."""
    rows: list[dict[str, Any]] = []
    with pgn_path.open("r", encoding="utf-8", errors="replace") as stream:
        game_no = 0
        while (game := chess.pgn.read_game(stream)) is not None:
            game_no += 1
            white_name = game.headers.get("White", "")
            black_name = game.headers.get("Black", "")
            if white_name == "NextGen":
                nextgen_color = chess.WHITE
            elif black_name == "NextGen":
                nextgen_color = chess.BLACK
            else:
                continue

            board = game.board()
            ply = 0
            for move in game.mainline_moves():
                ply += 1
                if board.turn == nextgen_color:
                    rows.append({
                        "game_no": game_no,
                        "game_id": game.headers.get("Round", str(game_no)),
                        "fen": board.fen(),
                        "played": move.uci(),
                        "played_san": board.san(move),
                        "ply": ply,
                        "fullmove": board.fullmove_number,
                        "color": "white" if board.turn == chess.WHITE else "black",
                        "phase": phase_of(board),
                        "result": game.headers.get("Result", "*"),
                        "termination": game.headers.get("Termination", ""),
                    })
                if move not in board.legal_moves:
                    raise ValueError(f"Illegal mainline move in game {game_no}: {move.uci()}")
                board.push(move)
    return rows


def score_cp(info: dict[str, Any], turn: chess.Color) -> int:
    score = info["score"].pov(turn).score(mate_score=100000)
    return int(score if score is not None else 0)


def pv_san(board: chess.Board, pv: list[chess.Move] | None, max_plies: int = 10) -> list[str]:
    copy = board.copy()
    san_line: list[str] = []
    for move in (pv or [])[:max_plies]:
        if move not in copy.legal_moves:
            break
        san_line.append(copy.san(move))
        copy.push(move)
    return san_line


def reason_flags(board: chess.Board, best: chess.Move, played: chess.Move) -> list[str]:
    reasons: list[str] = []
    if best == played:
        return ["engine_played_deep_best"]
    if board.is_capture(best) and not board.is_capture(played):
        reasons.append("missed_capture")
    if board.gives_check(best) and not board.gives_check(played):
        reasons.append("missed_check")
    if best.promotion:
        reasons.append("promotion_available")
    if board.is_check():
        reasons.append("defensive_move_while_in_check")
    if not reasons:
        reasons.append("positional_or_tactical_choice")
    return reasons


def analyse_position(engine, row: dict[str, Any], seconds: float) -> dict[str, Any]:
    board = chess.Board(row["fen"])
    played = chess.Move.from_uci(row["played"])
    if played not in board.legal_moves:
        raise ValueError(f"Recorded move {played.uci()} is illegal in FEN {row['fen']}")
    limit = chess.engine.Limit(time=seconds)
    # Stockfish chooses search depth based on the position and available time.
    best_info = engine.analyse(board, limit)
    best_pv = best_info.get("pv") or []
    if not best_pv:
        raise RuntimeError("Stockfish returned no principal variation")
    best_move = best_pv[0]
    played_info = engine.analyse(board, limit, root_moves=[played])
    best_cp = score_cp(best_info, board.turn)
    played_cp = score_cp(played_info, board.turn)
    loss = max(0, best_cp - played_cp)
    best_san = board.san(best_move)
    return {
        **row,
        "best": best_move.uci(),
        "best_san": best_san,
        "played_cp": played_cp,
        "best_cp": best_cp,
        "cp_loss": loss,
        "quick_or_deep_seconds": seconds,
        "best_depth": best_info.get("depth"),
        "best_seldepth": best_info.get("seldepth"),
        "best_nodes": best_info.get("nodes"),
        "played_depth": played_info.get("depth"),
        "played_nodes": played_info.get("nodes"),
        "best_pv_san": pv_san(board, best_pv),
        "played_pv_san": pv_san(board, played_info.get("pv")),
        "reasons": reason_flags(board, best_move, played),
        "stockfish_engine": "Stockfish 19 MAX (UCI strength limit disabled)",
    }


def severity(cp_loss: int) -> str:
    if cp_loss >= 800:
        return "catastrophic"
    if cp_loss >= 300:
        return "blunder"
    if cp_loss >= 150:
        return "mistake"
    if cp_loss >= 80:
        return "inaccuracy"
    return "below_threshold"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgn", type=Path, required=True)
    parser.add_argument("--stockfish", type=Path, default=DEFAULT_STOCKFISH)
    parser.add_argument("--out", type=Path, default=ROOT / "datasets" / "strict_weakness_report.json")
    parser.add_argument("--quick-seconds", type=float, default=0.35,
                        help="initial timed scan per candidate/best move; no fixed depth")
    parser.add_argument("--critical-seconds", type=float, default=10.0,
                        help="longer timed re-analysis per best/played move for selected critical positions")
    parser.add_argument("--threshold-cp", type=int, default=100)
    parser.add_argument("--top-critical", type=int, default=30)
    parser.add_argument("--hash-mb", type=int, default=256)
    args = parser.parse_args()

    if args.quick_seconds <= 0 or args.critical_seconds <= 0:
        parser.error("analysis time budgets must be > 0 seconds")
    if args.critical_seconds < args.quick_seconds:
        parser.error("--critical-seconds must be >= --quick-seconds")
    if args.threshold_cp < 0 or args.top_critical < 1 or args.hash_mb < 16:
        parser.error("threshold must be >=0, top-critical >=1, and hash-mb >=16")
    if not args.pgn.is_file():
        parser.error(f"PGN not found: {args.pgn}")
    if not args.stockfish.is_file():
        parser.error(f"Stockfish binary not found: {args.stockfish}")

    rows = collect_nextgen_positions(args.pgn)
    if not rows:
        parser.error("No NextGen mainline moves found. PGN headers must identify the player as 'NextGen'.")

    print(
        f"Auditing {len(rows)} NextGen moves from {len({r['game_no'] for r in rows})} games; "
        f"quick={args.quick_seconds}s, critical={args.critical_seconds}s; no fixed depth.",
        flush=True,
    )
    engine = chess.engine.SimpleEngine.popen_uci(str(args.stockfish))
    quick: list[dict[str, Any]] = []
    deep: list[dict[str, Any]] = []
    try:
        engine.configure({"Threads": 1, "Hash": args.hash_mb, "UCI_LimitStrength": False})
        for index, row in enumerate(rows, 1):
            try:
                record = analyse_position(engine, row, args.quick_seconds)
                record["severity"] = severity(record["cp_loss"])
                quick.append(record)
            except Exception as exc:
                quick.append({**row, "analysis_error": str(exc)})
            if index % 25 == 0 or index == len(rows):
                print(f"quick scan {index}/{len(rows)}", flush=True)

        candidates = [
            row for row in quick
            if "cp_loss" in row and row["cp_loss"] >= args.threshold_cp
        ]
        candidates.sort(key=lambda row: row["cp_loss"], reverse=True)
        chosen = candidates[:args.top_critical]
        print(f"Deep re-analysis of {len(chosen)} biggest candidate mistakes.", flush=True)
        # Stockfish again picks its own depth under a much larger time budget.
        # Only positions that look critical in the scan receive this expensive pass.
        for index, row in enumerate(chosen, 1):
            original = next((x for x in rows if x["game_no"] == row["game_no"] and x["ply"] == row["ply"]), None)
            if original is None:
                continue
            try:
                record = analyse_position(engine, original, args.critical_seconds)
                record["severity"] = severity(record["cp_loss"])
                record["deep_reanalysis"] = True
                deep.append(record)
            except Exception as exc:
                deep.append({**row, "deep_analysis_error": str(exc)})
            print(f"deep {index}/{len(chosen)}", flush=True)
    finally:
        try:
            engine.quit()
        except Exception:
            pass

    by_phase: dict[str, list[int]] = defaultdict(list)
    by_severity: Counter[str] = Counter()
    by_reason: Counter[str] = Counter()
    for row in quick:
        if "cp_loss" not in row:
            continue
        by_phase[row["phase"]].append(row["cp_loss"])
        by_severity[severity(row["cp_loss"])] += 1
        by_reason.update(row["reasons"])

    report = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "input_pgn": str(args.pgn),
        "stockfish_binary": str(args.stockfish),
        "strength_limit": "disabled (MAX)",
        "analysis_policy": {
            "quick_seconds_per_best_and_played_search": args.quick_seconds,
            "critical_seconds_per_best_and_played_search": args.critical_seconds,
            "depth_limit": None,
            "policy": "Timed iterative deepening; Stockfish selects search depth. Top apparent mistakes are re-evaluated with a larger time budget.",
        },
        "game_count": len({row["game_no"] for row in rows}),
        "nextgen_moves_scanned": len(rows),
        "quick_scan_failures": sum("analysis_error" in row for row in quick),
        "threshold_cp": args.threshold_cp,
        "apparent_mistakes_at_or_above_threshold": len(candidates),
        "deep_positions_requested": len(chosen),
        "deep_positions_completed": sum("deep_analysis_error" not in row for row in deep),
        "summary_by_phase": {
            phase: {
                "moves": len(values),
                "mean_cp_loss": round(statistics.fmean(values), 1) if values else 0.0,
                "median_cp_loss": round(statistics.median(values), 1) if values else 0.0,
                "cp_loss_300_plus": sum(value >= 300 for value in values),
                "cp_loss_800_plus": sum(value >= 800 for value in values),
            }
            for phase, values in by_phase.items()
        },
        "quick_severity_counts": dict(by_severity),
        "quick_reason_counts": dict(by_reason.most_common()),
        "top_quick_mistakes": [
            {key: value for key, value in row.items() if key not in ("fen",)}
            for row in candidates[:50]
        ],
        "deep_reanalysis": deep,
        "interpretation": (
            "Centipawn-loss scan is a diagnostic signal, not a definitive move-quality oracle. "
            "Search budgets differ from actual tournament play; review PVs and confirm proposed fixes with new matches."
        ),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "report": str(args.out),
        "nextgen_moves_scanned": report["nextgen_moves_scanned"],
        "apparent_mistakes_at_or_above_threshold": report["apparent_mistakes_at_or_above_threshold"],
        "deep_positions_completed": report["deep_positions_completed"],
        "summary_by_phase": report["summary_by_phase"],
        "quick_severity_counts": report["quick_severity_counts"],
    }, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
