#!/usr/bin/env python3
"""
Clock-controlled matches: NextGen vs Stockfish 19.
Default TC is 10+0.1 (10 seconds + 0.1s increment) — the engine-testing
equivalent of a blitz game and the only way to play *many* games on 2 cores.
True 10+0 (10 minutes/side) is ~15-20 min wall time per game.

Usage:
  python3 tools/clock_match.py --games 80 --tc 10+0.1 --sf-elo 1320
  python3 tools/clock_match.py --games 20 --tc 10+0.1 --sf-elo 0   # SF19 max
"""
from __future__ import annotations
import argparse, json, math, random, sys, time, traceback
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import chess, chess.engine, chess.pgn

ROOT = Path(__file__).resolve().parent.parent
OUR = ROOT / "bin" / "chess_engine"
def _find_sf() -> Path:
    for p in (ROOT / "bin" / "stockfish19", Path("/tmp/sf19/stockfish19"), Path("/tmp/stockfish19")):
        if p.exists():
            return p
    return ROOT / "bin" / "stockfish19"
SF = _find_sf()
DATA = ROOT / "datasets"
DATA.mkdir(exist_ok=True)
OUT = DATA / "clock_match.jsonl"
PGN = DATA / "clock_match.pgn"
SUMMARY = DATA / "clock_match_summary.json"

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


def parse_tc(s: str) -> tuple[float, float]:
    if "+" in s:
        a, b = s.split("+", 1)
        return float(a), float(b)
    return float(s), 0.0


def elo_from_score(opp, score, n):
    if n <= 0:
        return None
    p = (score * n + 0.5) / (n + 1.0)
    p = min(max(p, 1e-6), 1 - 1e-6)
    return opp - 400.0 * math.log10(1.0 / p - 1.0)


def start_our():
    e = chess.engine.SimpleEngine.popen_uci(str(OUR))
    e.configure({"Hash": 32, "Threads": 1})
    # experience book if present
    book = ROOT / "datasets" / "experience_book.txt"
    if book.exists():
        try:
            e.configure({"ExperienceFile": str(book), "OwnBook": True})
        except Exception:
            pass
    return e


def start_sf(elo: int | None):
    e = chess.engine.SimpleEngine.popen_uci(str(SF))
    e.configure({"Hash": 16, "Threads": 1})
    if elo and elo > 0:
        e.configure({"UCI_LimitStrength": True, "UCI_Elo": int(elo)})
    else:
        try:
            e.configure({"UCI_LimitStrength": False})
        except Exception:
            pass
    return e


def safe_quit(e):
    if not e:
        return
    try:
        e.quit()
    except Exception:
        try:
            e.protocol.process.kill()
        except Exception:
            pass


def play(our, sf, our_white, opening, base, inc, max_ply=180):
    board = chess.Board()
    if opening:
        for tok in opening.split():
            board.push_uci(tok)
    wtime = btime = base
    moves = []
    crashed = None
    while not board.is_game_over(claim_draw=True) and board.ply() < max_ply:
        is_our = (board.turn == chess.WHITE) == our_white
        eng = our if is_our else sf
        limit = chess.engine.Limit(
            white_clock=max(0.05, wtime),
            black_clock=max(0.05, btime),
            white_inc=inc,
            black_inc=inc,
        )
        t0 = time.time()
        try:
            res = eng.play(board, limit)
        except Exception as e:
            crashed = f"{'nextgen' if is_our else 'sf19'}: {e}"
            break
        used = time.time() - t0
        if res.move is None or res.move not in board.legal_moves:
            crashed = f"{'nextgen' if is_our else 'sf19'} illegal {res.move}"
            break
        if board.turn == chess.WHITE:
            wtime = max(0.0, wtime - used + inc)
        else:
            btime = max(0.0, btime - used + inc)
        moves.append(res.move.uci())
        board.push(res.move)
        if wtime <= 0 or btime <= 0:
            break

    if crashed:
        if crashed.startswith("nextgen"):
            result = "0-1" if our_white else "1-0"
        else:
            result = "1-0" if our_white else "0-1"
        term = "crash:" + crashed
    elif wtime <= 0:
        result, term = "0-1", "white_flag"
    elif btime <= 0:
        result, term = "1-0", "black_flag"
    elif board.is_game_over(claim_draw=True):
        result = board.result(claim_draw=True)
        oc = board.outcome(claim_draw=True)
        term = oc.termination.name if oc else "game_over"
    else:
        result, term = "1/2-1/2", "max_ply"

    if result == "1-0":
        our_res = "win" if our_white else "loss"
    elif result == "0-1":
        our_res = "loss" if our_white else "win"
    else:
        our_res = "draw"
    return {
        "result": result,
        "our_result": our_res,
        "our_white": our_white,
        "opening": opening,
        "ply": board.ply(),
        "termination": term,
        "moves": moves,
        "final_fen": board.fen(),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=60)
    ap.add_argument("--tc", default="10+0.1")
    ap.add_argument("--sf-elo", type=int, default=1320, help="0 = SF19 max")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()
    base, inc = parse_tc(args.tc)
    elo = args.sf_elo if args.sf_elo > 0 else None
    label = f"Elo{elo}" if elo else "MAX"
    print(f"=== NextGen vs SF19 {label}  TC {args.tc}  games={args.games} ===", flush=True)

    wins = losses = draws = discarded = 0
    our = sf = None

    def restart():
        nonlocal our, sf
        safe_quit(our)
        safe_quit(sf)
        time.sleep(0.2)
        our = start_our()
        sf = start_sf(elo)
        return our, sf

    restart()
    t0 = time.time()
    try:
        for i in range(args.games):
            our_white = i % 2 == 0
            opening = OPENINGS[i % len(OPENINGS)]
            try:
                g = play(our, sf, our_white, opening, base, inc)
            except Exception as e:
                print(f"  hard fail {e}", flush=True)
                traceback.print_exc()
                discarded += 1
                restart()
                continue
            if g["our_result"] == "win":
                wins += 1
            elif g["our_result"] == "loss":
                losses += 1
            else:
                draws += 1
            rec = {
                "ts": datetime.now(timezone.utc).isoformat(),
                "tc": args.tc,
                "sf_elo": elo or "max",
                "tag": args.tag,
                **{k: g[k] for k in ("result", "our_result", "our_white", "opening", "ply", "termination")},
            }
            with OUT.open("a") as f:
                f.write(json.dumps(rec) + "\n")
            n = wins + losses + draws
            score = (wins + 0.5 * draws) / n if n else 0
            print(
                f"  {i+1}/{args.games} {'W' if our_white else 'B'} {g['our_result']:4s} "
                f"{g['result']} ply={g['ply']:3d} {g['termination'][:22]:22s} "
                f"{wins}W {losses}L {draws}D  score={score:.3f}",
                flush=True,
            )
            if (i + 1) % 20 == 0:
                restart()
    finally:
        safe_quit(our)
        safe_quit(sf)

    n = wins + losses + draws
    score = (wins + 0.5 * draws) / n if n else 0
    opp = elo if elo else 3620
    est = elo_from_score(opp, score, n) if n else None
    summary = {
        "tc": args.tc,
        "sf": label,
        "games": n,
        "wins": wins,
        "losses": losses,
        "draws": draws,
        "discarded": discarded,
        "score": score,
        "est_elo": est,
        "seconds": time.time() - t0,
    }
    prev = []
    if SUMMARY.exists():
        try:
            prev = json.loads(SUMMARY.read_text())
            if isinstance(prev, dict):
                prev = [prev]
        except Exception:
            prev = []
    prev.append(summary)
    SUMMARY.write_text(json.dumps(prev, indent=2))
    print("\nFINAL", json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
