#!/usr/bin/env python3
"""
CuteChess automation + SPRT + Elo estimation - Stockfish-grade benchmark
"""
import subprocess, json, argparse, re, os, sys, time
from pathlib import Path

def run_tournament(engine1, engine2, games=100, tc="10+0.1", book=""):
    cmd = [
        "cutechess-cli",
        "-engine", f"cmd={engine1} proto=uci",
        "-engine", f"cmd={engine2} proto=uci",
        "-each", f"tc={tc} proto=uci",
        "-games", str(games),
        "-rounds", "1",
        "-repeat",
        "-ratinginterval", "10",
        "-pgnout", "games.pgn"
    ]
    if book:
        cmd += ["-openings", f"file={book} format=pgn order=random"]
    print(" ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    print(proc.stdout[-2000:])
    # parse Elo
    m = re.search(r"Elo diff:\s*([-\d\.]+)", proc.stdout)
    elo = float(m.group(1)) if m else 0.0
    return elo

def sprt(elo0=0, elo1=5, alpha=0.05, beta=0.05, engine1="", engine2=""):
    # Sequential Probability Ratio Test
    # Using cutechess-cli sprt
    cmd = [
        "cutechess-cli",
        "-engine", f"cmd={engine1} proto=uci",
        "-engine", f"cmd={engine2} proto=uci",
        "-each", "tc=10+0.1",
        "-sprt", f"elo0={elo0} elo1={elo1} alpha={alpha} beta={beta}",
        "-games", "4000"
    ]
    subprocess.run(cmd)

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine1", required=True)
    ap.add_argument("--engine2", required=True)
    ap.add_argument("--games", type=int, default=100)
    ap.add_argument("--tc", default="10+0.1")
    ap.add_argument("--book", default="")
    ap.add_argument("--sprt", action="store_true")
    args = ap.parse_args()
    if args.sprt:
        sprt(engine1=args.engine1, engine2=args.engine2)
    else:
        elo = run_tournament(args.engine1, args.engine2, args.games, args.tc, args.book)
        print(f"Elo diff: {elo}")
