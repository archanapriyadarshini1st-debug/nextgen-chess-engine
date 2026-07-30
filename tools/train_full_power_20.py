#!/usr/bin/env python3
"""Train with Stockfish 18 full power depth 20 - quality not quantity, 4060 rated loop"""
import chess, chess.engine, json, random, time
from pathlib import Path

STOCKFISH="bin/stockfish18"
OUT="datasets/full_power_20.jsonl"

sf=chess.engine.SimpleEngine.popen_uci(STOCKFISH)
sf.configure({"Hash":128,"Threads":1})

Path(OUT).parent.mkdir(parents=True, exist_ok=True)

count=0
for i in range(2000):
    board=chess.Board()
    for _ in range(random.randint(8,15)):
        if board.is_game_over():
            break
        board.push(random.choice(list(board.legal_moves)))
    if board.is_game_over():
        continue
    try:
        info=sf.analyse(board, chess.engine.Limit(depth=20))
        score=info["score"].relative
        cp=score.score(mate_score=10000)
        if cp is None:
            cp=10000 if score.mate() and score.mate()>0 else -10000
        cp=max(-2000,min(2000,cp))
        pv=info.get("pv",[])
        best=pv[0] if pv else None
        entry={
            "fen":board.fen(),
            "sf_best":best.uci() if best else None,
            "sf_eval_cp":cp,
            "depth":20,
            "stockfish_strength":"full 4060",
            "tag":"full_power_20"
        }
        with open(OUT,"a") as f:
            f.write(json.dumps(entry)+"\n")
        count+=1
        if count%100==0:
            print(f"Generated {count} positions at depth 20 full power, last cp {cp}")
    except Exception as e:
        print(f"Error {e}")
        continue

sf.quit()
print(f"Done {count} positions full power depth 20 to {OUT}")
