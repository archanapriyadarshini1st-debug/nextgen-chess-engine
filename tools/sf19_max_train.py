#!/usr/bin/env python3
"""
NextGen vs Stockfish 19 (full strength) — measure Elo, play games, and
after every loss save the best moves NextGen could have played.

Usage:
  python tools/sf19_max_train.py elo --games-per-level 12
  python tools/sf19_max_train.py train --games 200
  python tools/sf19_max_train.py both --games 200
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
import time
import traceback
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

import chess
import chess.engine
import chess.pgn

ROOT = Path(__file__).resolve().parent.parent
OUR = ROOT / "bin" / "chess_engine"
def _find_sf() -> Path:
    for p in (ROOT / "bin" / "stockfish19", Path("/tmp/sf19/stockfish19"), Path("/tmp/stockfish19")):
        if p.exists():
            return p
    return ROOT / "bin" / "stockfish19"
SF = _find_sf()
DATA = ROOT / "datasets"
DATA.mkdir(parents=True, exist_ok=True)

MISSED = DATA / "missed_best_moves.jsonl"
EXPERIENCE = DATA / "experience_book.txt"
GAMES_JSONL = DATA / "games_vs_sf19.jsonl"
PGN_PATH = DATA / "games_vs_sf19.pgn"
WEAKNESS = DATA / "weaknesses.json"
ELO_JSON = DATA / "elo_measurement.json"
STATUS = DATA / "train_status.json"

# Diverse openings so we don't just lose the same e4/a4 game 200 times.
OPENINGS = [
    "",  # startpos
    "e2e4 e7e5",
    "e2e4 c7c5",
    "e2e4 e7e6",
    "e2e4 c7c6",
    "e2e4 d7d5",
    "d2d4 d7d5",
    "d2d4 g8f6",
    "d2d4 g8f6 c2c4 e7e6",
    "d2d4 g8f6 c2c4 g7g6",
    "g1f3 d7d5",
    "c2c4 e7e5",
    "e2e4 e7e5 g1f3 b8c6 f1b5",
    "e2e4 c7c5 g1f3 d7d6 d2d4",
    "e2e4 e7e5 g1f3 b8c6 d2d4",
    "d2d4 d7d5 c2c4 e7e6",
    "e2e4 g8f6",
    "g1f3 g8f6",
    "e2e4 e7e5 g1f3 g8f6",
    "d2d4 d7d5 g1f3 g8f6",
]

SF_ELO_LEVELS = [1320, 1400, 1600, 1800, 2000]


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def append_jsonl(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")
        f.flush()


def save_json(path: Path, obj) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, ensure_ascii=False))
    tmp.replace(path)


def phase_of(board: chess.Board) -> str:
    ply = board.ply()
    n = len(board.piece_map())
    if ply < 20:
        return "opening"
    if n <= 10 or ply >= 80:
        return "endgame"
    return "middlegame"


def piece_name(board: chess.Board, move: chess.Move) -> str:
    p = board.piece_at(move.from_square)
    return p.symbol().upper() if p else "?"


def explain_best(board: chess.Board, best: chess.Move, played: chess.Move | None) -> str:
    why = []
    if board.is_en_passant(best) or board.is_capture(best):
        cap = board.piece_at(best.to_square)
        why.append(f"wins {cap.symbol() if cap else 'ep-pawn'}")
    if board.gives_check(best):
        why.append("gives check")
    if board.is_castling(best):
        why.append("castles (king safety)")
    if best.promotion:
        why.append("promotes")
    # centre
    to_file = chess.square_file(best.to_square)
    to_rank = chess.square_rank(best.to_square)
    if to_file in (3, 4) and to_rank in (3, 4):
        why.append("occupies centre")
    # development
    p = board.piece_at(best.from_square)
    if p and p.piece_type in (chess.KNIGHT, chess.BISHOP) and board.ply() < 16:
        why.append("develops a piece")
    # hanging reply
    board.push(best)
    if board.is_checkmate():
        why.append("checkmate")
    elif board.is_check():
        why.append("forcing")
    board.pop()
    if played and played != best:
        if board.is_capture(best) and not board.is_capture(played):
            why.append("you missed a capture")
        if board.gives_check(best) and not board.gives_check(played):
            why.append("you missed a check")
        pp = board.piece_at(played.from_square)
        if pp and pp.piece_type == chess.PAWN and chess.square_file(played.from_square) in (0, 7):
            why.append("you pushed a wing pawn instead")
    if not why:
        why.append("positional improvement")
    return "; ".join(why)


def elo_from_score(opp_elo: float, score: float, games: int) -> float | None:
    """Maximum-likelihood Elo given score in [0,1] vs a known opponent."""
    if games <= 0:
        return None
    # avoid 0/1 infinities with a tiny Jeffreys correction
    s = (score * games + 0.5) / (games + 1.0)
    s = min(max(s, 1e-6), 1 - 1e-6)
    return opp_elo - 400.0 * math.log10(1.0 / s - 1.0)


def wilson_interval(wins: float, n: int, z: float = 1.96) -> tuple[float, float]:
    if n <= 0:
        return (0.0, 1.0)
    p = wins / n
    den = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt((p * (1 - p) + z * z / (4 * n)) / n) / den
    return (max(0.0, centre - half), min(1.0, centre + half))


def start_engine(path: Path, hash_mb: int = 16, threads: int = 1, limit_elo: int | None = None):
    eng = chess.engine.SimpleEngine.popen_uci(str(path))
    opts = {"Hash": hash_mb, "Threads": threads}
    try:
        eng.configure(opts)
    except Exception:
        pass
    if limit_elo is not None:
        try:
            eng.configure({"UCI_LimitStrength": True, "UCI_Elo": int(limit_elo)})
        except Exception as e:
            print(f"  warn: could not set UCI_Elo={limit_elo}: {e}")
    else:
        try:
            eng.configure({"UCI_LimitStrength": False})
        except Exception:
            pass
    return eng


def safe_quit(eng) -> None:
    if eng is None:
        return
    try:
        eng.quit()
    except Exception:
        try:
            eng.protocol.process.kill()
        except Exception:
            pass


def apply_opening(board: chess.Board, opening: str) -> None:
    if not opening:
        return
    for tok in opening.split():
        board.push_uci(tok)


def play_game(
    our,
    sf,
    *,
    our_white: bool,
    opening: str,
    our_limit: chess.engine.Limit,
    sf_limit: chess.engine.Limit,
    max_ply: int = 160,
) -> dict:
    board = chess.Board()
    apply_opening(board, opening)
    moves = []
    our_moves = []  # (fen_before, move_uci, ply)
    crashed = None
    while not board.is_game_over(claim_draw=True) and board.ply() < max_ply:
        is_our = (board.turn == chess.WHITE and our_white) or (
            board.turn == chess.BLACK and not our_white
        )
        engine = our if is_our else sf
        limit = our_limit if is_our else sf_limit
        fen_before = board.fen()
        try:
            result = engine.play(board, limit)
        except Exception as e:
            crashed = f"{'nextgen' if is_our else 'sf19'}: {e}"
            break
        if result.move is None or result.move not in board.legal_moves:
            crashed = f"{'nextgen' if is_our else 'sf19'} illegal/null {result.move}"
            break
        if is_our:
            our_moves.append(
                {
                    "fen": fen_before,
                    "played": result.move.uci(),
                    "ply": board.ply(),
                    "phase": phase_of(board),
                    "piece": piece_name(board, result.move),
                }
            )
        moves.append(result.move.uci())
        board.push(result.move)

    if crashed:
        # treat crash/illegal as a loss for the crashing side
        if crashed.startswith("nextgen"):
            result_str = "0-1" if our_white else "1-0"
        else:
            result_str = "1-0" if our_white else "0-1"
        termination = "crash:" + crashed
    elif board.is_game_over(claim_draw=True):
        result_str = board.result(claim_draw=True)
        termination = board.outcome(claim_draw=True).termination.name if board.outcome(claim_draw=True) else "game_over"
    else:
        result_str = "1/2-1/2"
        termination = "max_ply"

    if result_str == "1-0":
        our_result = "win" if our_white else "loss"
    elif result_str == "0-1":
        our_result = "loss" if our_white else "win"
    else:
        our_result = "draw"

    return {
        "result": result_str,
        "our_result": our_result,
        "our_white": our_white,
        "opening": opening,
        "moves": moves,
        "our_moves": our_moves,
        "ply": board.ply(),
        "termination": termination,
        "final_fen": board.fen(),
        "pgn_moves": " ".join(moves),
    }


def analyse_our_moves(sf, game: dict, *, depth: int = 10, min_cp_loss: int = 50) -> list[dict]:
    """After a loss (or any game), ask SF19 for the move NextGen should have played."""
    missed = []
    for rec in game["our_moves"]:
        board = chess.Board(rec["fen"])
        played = chess.Move.from_uci(rec["played"])
        if played not in board.legal_moves:
            continue
        try:
            info = sf.analyse(board, chess.engine.Limit(depth=depth))
        except Exception:
            continue
        pv = info.get("pv") or []
        if not pv:
            continue
        best = pv[0]
        pov = info["score"].pov(board.turn)
        best_cp = pov.score(mate_score=10000)
        if best_cp is None:
            continue
        # eval of the move we actually played
        try:
            info2 = sf.analyse(board, chess.engine.Limit(depth=max(6, depth - 2)), root_moves=[played])
            played_cp = info2["score"].pov(board.turn).score(mate_score=10000)
            if played_cp is None:
                played_cp = best_cp
        except Exception:
            played_cp = best_cp
        cp_loss = int(best_cp - played_cp)
        if cp_loss < min_cp_loss and best == played:
            continue
        if best == played:
            continue
        why = explain_best(board, best, played)
        entry = {
            "ts": utc_now(),
            "game_id": game.get("game_id"),
            "fen": rec["fen"],
            "ply": rec["ply"],
            "phase": rec["phase"],
            "piece": rec["piece"],
            "played": rec["played"],
            "best": best.uci(),
            "best_cp": int(best_cp),
            "played_cp": int(played_cp),
            "cp_loss": int(cp_loss),
            "why_best": why,
            "when_to_play": rec["phase"],
            "our_result": game["our_result"],
            "severity": (
                "blunder"
                if cp_loss >= 300
                else "mistake"
                if cp_loss >= 100
                else "inaccuracy"
            ),
        }
        missed.append(entry)
    return missed


def write_experience_line(fen: str, best: str, weight: int) -> None:
    weight = max(1, min(int(weight), 65535))
    with EXPERIENCE.open("a", encoding="utf-8") as f:
        f.write(f"{fen} {best} {weight}\n")
        f.flush()


def update_weaknesses(missed: list[dict], games_meta: list[dict]) -> dict:
    by_phase = defaultdict(lambda: {"n": 0, "cp": 0, "blunders": 0})
    by_piece = Counter()
    by_why = Counter()
    by_severity = Counter()
    opening_played = Counter()
    for m in missed:
        ph = m.get("phase", "?")
        by_phase[ph]["n"] += 1
        by_phase[ph]["cp"] += m.get("cp_loss", 0)
        if m.get("severity") == "blunder":
            by_phase[ph]["blunders"] += 1
        by_piece[m.get("piece", "?")] += 1
        by_why[m.get("why_best", "")] += 1
        by_severity[m.get("severity", "")] += 1
    results = Counter(g.get("our_result") for g in games_meta)
    report = {
        "updated": utc_now(),
        "games": len(games_meta),
        "results": dict(results),
        "missed_moves": len(missed),
        "by_phase": {
            k: {
                "moves": v["n"],
                "avg_cp_loss": (v["cp"] / v["n"] if v["n"] else 0),
                "blunders": v["blunders"],
            }
            for k, v in by_phase.items()
        },
        "blunders_by_piece": dict(by_piece),
        "top_reasons": by_why.most_common(12),
        "severity": dict(by_severity),
    }
    save_json(WEAKNESS, report)
    return report


def write_pgn(game: dict, white_name: str, black_name: str) -> None:
    board = chess.Board()
    apply_opening(board, game.get("opening") or "")
    g = chess.pgn.Game.from_board(chess.Board())
    # rebuild from move list
    node = g
    b = chess.Board()
    apply_opening(b, game.get("opening") or "")
    # if opening, add those moves first
    if game.get("opening"):
        tmp = chess.Board()
        for tok in game["opening"].split():
            mv = chess.Move.from_uci(tok)
            node = node.add_variation(mv)
            tmp.push(mv)
        b = tmp
    for u in game["moves"]:
        node = node.add_variation(chess.Move.from_uci(u))
    g.headers["Event"] = "NextGen vs Stockfish 19"
    g.headers["White"] = white_name
    g.headers["Black"] = black_name
    g.headers["Result"] = game["result"]
    g.headers["Termination"] = game.get("termination", "")
    g.headers["Date"] = datetime.now().strftime("%Y.%m.%d")
    with PGN_PATH.open("a", encoding="utf-8") as f:
        print(g, file=f, end="\n\n")
        f.flush()


def cmd_elo(args) -> dict:
    print(f"\n=== Elo ladder vs Stockfish 19 (UCI_LimitStrength) ===")
    print(f"NextGen: {OUR}")
    print(f"Stockfish: {SF}")
    n = args.games_per_level
    our_ms = args.our_ms
    sf_ms = args.sf_ms
    ladder = []
    for elo in SF_ELO_LEVELS:
        print(f"\n-- vs SF19 Elo {elo}  ({n} games, {our_ms}ms vs {sf_ms}ms) --")
        our = sf = None
        wins = losses = draws = 0
        discarded = 0
        try:
            our = start_engine(OUR, hash_mb=32, threads=1)
            sf = start_engine(SF, hash_mb=16, threads=1, limit_elo=elo)
            our_limit = chess.engine.Limit(time=our_ms / 1000.0)
            sf_limit = chess.engine.Limit(time=sf_ms / 1000.0)
            for i in range(n):
                our_white = i % 2 == 0
                opening = OPENINGS[i % len(OPENINGS)]
                try:
                    g = play_game(
                        our,
                        sf,
                        our_white=our_white,
                        opening=opening,
                        our_limit=our_limit,
                        sf_limit=sf_limit,
                    )
                except Exception as e:
                    print(f"  game {i+1} exception {e}")
                    discarded += 1
                    safe_quit(our)
                    safe_quit(sf)
                    time.sleep(0.3)
                    our = start_engine(OUR, hash_mb=32, threads=1)
                    sf = start_engine(SF, hash_mb=16, threads=1, limit_elo=elo)
                    continue
                if g["our_result"] == "win":
                    wins += 1
                elif g["our_result"] == "loss":
                    losses += 1
                else:
                    draws += 1
                print(
                    f"  {i+1}/{n} {'W' if our_white else 'B'} {g['our_result']:4s} "
                    f"{g['result']} ply={g['ply']} term={g['termination'][:24]} "
                    f"({wins}W {losses}L {draws}D)"
                )
        finally:
            safe_quit(our)
            safe_quit(sf)

        total = wins + losses + draws
        score = (wins + 0.5 * draws) / total if total else 0.0
        est = elo_from_score(elo, score, total) if total else None
        lo, hi = wilson_interval(wins + 0.5 * draws, total) if total else (0, 1)
        row = {
            "sf_elo": elo,
            "games": total,
            "wins": wins,
            "losses": losses,
            "draws": draws,
            "discarded": discarded,
            "score": score,
            "estimated_elo": est,
            "score_ci95": [lo, hi],
        }
        ladder.append(row)
        est_s = f"{est:.0f}" if est is not None else "n/a"
        print(
            f"  RESULT vs {elo}: {wins}W {losses}L {draws}D  "
            f"score={score:.3f}  est_elo={est_s}"
        )

    # Combine independent estimates with inverse-variance-ish weighting by games
    weighted = []
    for row in ladder:
        if row["estimated_elo"] is None or row["games"] < 4:
            continue
        # more informative near 50% — weight by games * p * (1-p)
        p = min(max(row["score"], 0.02), 0.98)
        w = row["games"] * p * (1 - p)
        weighted.append((row["estimated_elo"], max(w, 0.1)))
    if weighted:
        combined = sum(e * w for e, w in weighted) / sum(w for _, w in weighted)
    else:
        combined = None

    out = {
        "measured_at": utc_now(),
        "opponent": "Stockfish 19",
        "time_control": f"{our_ms}ms vs {sf_ms}ms per move",
        "ladder": ladder,
        "combined_elo_estimate": combined,
        "notes": (
            "Stockfish 19 UCI_Elo is calibrated 1320–3190. Full-strength SF19 is "
            "about 3600 CCRL; a 0% score vs max only says NextGen << 3000. The "
            "ladder vs limited SF19 is the honest rating."
        ),
    }
    save_json(ELO_JSON, out)
    print(f"\nCombined Elo estimate: {combined:.0f}" if combined else "\nCombined Elo: n/a")
    print(f"Wrote {ELO_JSON}")
    return out


def cmd_train(args) -> None:
    n_games = args.games
    our_ms = args.our_ms
    sf_ms = args.sf_ms
    depth = args.analyse_depth
    print(f"\n=== Train vs Stockfish 19 MAX (UCI_LimitStrength=false) ===")
    print(f"games={n_games}  NextGen {our_ms}ms  SF19 {sf_ms}ms  analyse depth {depth}")
    print(f"missed moves -> {MISSED}")
    print(f"experience book -> {EXPERIENCE}")

    wins = losses = draws = 0
    discarded = 0
    missed_total = 0
    our = sf = None
    t0 = time.time()

    def restart():
        nonlocal our, sf
        safe_quit(our)
        safe_quit(sf)
        time.sleep(0.2)
        our = start_engine(OUR, hash_mb=32, threads=1)
        sf = start_engine(SF, hash_mb=16, threads=1, limit_elo=None)
        return our, sf

    restart()
    our_limit = chess.engine.Limit(time=our_ms / 1000.0)
    sf_limit = chess.engine.Limit(time=sf_ms / 1000.0)
    games_meta = []

    try:
        for i in range(n_games):
            our_white = i % 2 == 0
            opening = OPENINGS[i % len(OPENINGS)]
            game_id = f"g{i+1:06d}"
            try:
                g = play_game(
                    our,
                    sf,
                    our_white=our_white,
                    opening=opening,
                    our_limit=our_limit,
                    sf_limit=sf_limit,
                )
            except Exception as e:
                print(f"  game {i+1} crashed hard: {e}")
                traceback.print_exc()
                discarded += 1
                restart()
                continue

            g["game_id"] = game_id
            g["ts"] = utc_now()
            g["opponent"] = "Stockfish 19 MAX"
            if g["our_result"] == "win":
                wins += 1
            elif g["our_result"] == "loss":
                losses += 1
            else:
                draws += 1

            # After a loss (and also draws with mistakes), ask SF19 what we should have played.
            new_missed = []
            if g["our_result"] in ("loss", "draw") or args.analyse_all:
                try:
                    new_missed = analyse_our_moves(
                        sf, g, depth=depth, min_cp_loss=args.min_cp_loss
                    )
                except Exception as e:
                    print(f"  analyse failed: {e}")
                    restart()

            for m in new_missed:
                append_jsonl(MISSED, m)
                # persist into the engine experience book so next game can reuse it
                if m["cp_loss"] >= args.min_cp_loss:
                    write_experience_line(m["fen"], m["best"], 50 + m["cp_loss"])
            missed_total += len(new_missed)

            rec = {
                "game_id": game_id,
                "our_white": our_white,
                "opening": opening,
                "result": g["result"],
                "our_result": g["our_result"],
                "ply": g["ply"],
                "termination": g["termination"],
                "missed": len(new_missed),
                "blunders": sum(1 for m in new_missed if m["severity"] == "blunder"),
            }
            append_jsonl(GAMES_JSONL, rec)
            games_meta.append(rec)
            try:
                write_pgn(
                    g,
                    "NextGenChessEngine" if our_white else "Stockfish 19",
                    "Stockfish 19" if our_white else "NextGenChessEngine",
                )
            except Exception:
                pass

            elapsed = time.time() - t0
            gps = (i + 1) / elapsed if elapsed else 0
            eta = (n_games - i - 1) / gps if gps else 0
            print(
                f"  {i+1}/{n_games} {'W' if our_white else 'B'} {g['our_result']:4s} "
                f"{g['result']} ply={g['ply']:3d} missed={len(new_missed):3d} "
                f"score {wins}W {losses}L {draws}D  "
                f"{gps:.2f} g/s  eta {eta/60:.1f}m"
            )
            if (i + 1) % 5 == 0:
                save_json(
                    STATUS,
                    {
                        "updated": utc_now(),
                        "played": i + 1,
                        "target": n_games,
                        "wins": wins,
                        "losses": losses,
                        "draws": draws,
                        "discarded": discarded,
                        "missed_moves": missed_total,
                        "games_per_sec": gps,
                    },
                )
            # restart engines every 25 games to contain leaks
            if (i + 1) % 25 == 0:
                restart()
    finally:
        safe_quit(our)
        safe_quit(sf)

    # weakness rollup from the jsonl (whole file, so resumes accumulate)
    all_missed = []
    if MISSED.exists():
        with MISSED.open(encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        all_missed.append(json.loads(line))
                    except Exception:
                        pass
    all_games = []
    if GAMES_JSONL.exists():
        with GAMES_JSONL.open(encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        all_games.append(json.loads(line))
                    except Exception:
                        pass
    report = update_weaknesses(all_missed, all_games)
    total = wins + losses + draws
    score = (wins + 0.5 * draws) / total if total else 0.0
    # SF19 max ~ 3620 CCRL 40/15; this is a lower bound statement
    sf19_max_elo = 3620
    est_vs_max = elo_from_score(sf19_max_elo, score, total) if total else None
    print("\n=== training batch done ===")
    print(f"  {wins}W {losses}L {draws}D  score={score:.3f}  vs SF19 max (~{sf19_max_elo})")
    if est_vs_max:
        print(f"  naive Elo vs max: {est_vs_max:.0f}  (unreliable if score is near 0)")
    print(f"  missed best-moves saved: {missed_total} this run, {len(all_missed)} all-time")
    print(f"  experience book: {EXPERIENCE} ({EXPERIENCE.stat().st_size if EXPERIENCE.exists() else 0} bytes)")
    print(f"  weaknesses: {json.dumps(report.get('by_phase', {}), indent=2)}")
    save_json(
        STATUS,
        {
            "updated": utc_now(),
            "played": n_games,
            "wins": wins,
            "losses": losses,
            "draws": draws,
            "score": score,
            "est_elo_vs_sf19_max": est_vs_max,
            "missed_moves_run": missed_total,
            "missed_moves_all": len(all_missed),
            "weaknesses": report,
        },
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["elo", "train", "both"], default="both", nargs="?")
    ap.add_argument("--games", type=int, default=80, help="games vs SF19 MAX")
    ap.add_argument("--games-per-level", type=int, default=10)
    ap.add_argument("--our-ms", type=int, default=120)
    ap.add_argument("--sf-ms", type=int, default=50)
    ap.add_argument("--analyse-depth", type=int, default=10)
    ap.add_argument("--min-cp-loss", type=int, default=50)
    ap.add_argument("--analyse-all", action="store_true")
    args = ap.parse_args()

    if not OUR.exists():
        print(f"missing engine {OUR}", file=sys.stderr)
        sys.exit(1)
    if not SF.exists():
        print(f"missing stockfish {SF}", file=sys.stderr)
        sys.exit(1)

    if args.mode in ("elo", "both"):
        cmd_elo(args)
    if args.mode in ("train", "both"):
        cmd_train(args)


if __name__ == "__main__":
    main()
