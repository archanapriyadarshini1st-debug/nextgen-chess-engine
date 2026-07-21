#!/usr/bin/env python3
"""
Distillation from Stockfish 18 -> NextGen training data
- Takes existing games.jsonl (our engine eval) and re-evaluates each FEN with Stockfish 18 at depth 12
- Stores Stockfish cp as new target for NNUE training
- This is how Stockfish itself trains: uses strong engine eval as teacher
"""

import subprocess, json, argparse, time
from pathlib import Path

STOCKFISH = "/tmp/stockfish/stockfish-ubuntu-x86-64-avx2"

def start_stockfish(path):
    proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    proc.stdin.write("uci\n")
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if "uciok" in line:
            break
    proc.stdin.write("isready\n")
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if "readyok" in line:
            break
    return proc

def eval_fen(proc, fen, depth=12):
    proc.stdin.write(f"position fen {fen}\n")
    proc.stdin.write(f"go depth {depth}\n")
    proc.stdin.flush()
    score=None
    best=None
    while True:
        line = proc.stdout.readline()
        if not line:
            break
        if "score cp" in line:
            try:
                parts=line.split()
                idx=parts.index("cp")
                score=int(parts[idx+1])
            except:
                pass
        if line.startswith("bestmove"):
            best=line.split()[1]
            break
    return score, best

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--input", default="datasets/games.jsonl")
    ap.add_argument("--output", default="datasets/distilled_sf18.jsonl")
    ap.add_argument("--stockfish", default=STOCKFISH)
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--limit", type=int, default=1000)
    args=ap.parse_args()

    proc = start_stockfish(args.stockfish)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    count=0
    with open(args.input) as f, open(out, "w") as outf:
        for line in f:
            if count>=args.limit:
                break
            try:
                e=json.loads(line)
                fen=e.get("fen")
                if not fen:
                    continue
                sf_score, sf_best = eval_fen(proc, fen, args.depth)
                if sf_score is None:
                    continue
                # Create new entry with SF eval as target
                new_e = e.copy()
                new_e["sf_eval_cp"] = sf_score
                new_e["sf_best"] = sf_best
                new_e["eval_cp"] = sf_score  # use SF as primary target for distillation
                new_e["tag"] = "distilled_sf18"
                outf.write(json.dumps(new_e)+"\n")
                count+=1
                if count%100==0:
                    print(f"Distilled {count}: {fen[:40]} -> cp {sf_score}")
            except Exception as ex:
                print(f"Error {ex}")
                continue

    proc.stdin.write("quit\n")
    proc.stdin.flush()
    proc.terminate()
    print(f"Done distilled {count} positions to {out}")

if __name__=="__main__":
    main()
