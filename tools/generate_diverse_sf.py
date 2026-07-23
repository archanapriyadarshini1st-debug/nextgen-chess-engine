#!/usr/bin/env python3
"""Generate diverse positions with Stockfish 18 eval - fixes training data problem"""
import chess, chess.engine, random, json
from pathlib import Path
SF="bin/stockfish18"
OUT="datasets/diverse_sf18.jsonl"

def random_opening_moves(board, n=6):
    for _ in range(n):
        moves=list(board.legal_moves)
        if not moves:
            break
        board.push(random.choice(moves))
    return board

sf=chess.engine.SimpleEngine.popen_uci(SF)
sf.configure({"Hash":32,"Threads":1})

Path(OUT).parent.mkdir(parents=True, exist_ok=True)
count=0
with open(OUT,"w") as out:
    for i in range(3000):
        board=chess.Board()
        n=random.randint(4,12)
        board = random_opening_moves(board, n)
        if board.is_game_over():
            continue
        try:
            info=sf.analyse(board, chess.engine.Limit(depth=12))
            score=info["score"].relative
            cp=score.score(mate_score=10000)
            if cp is None:
                if score.is_mate():
                    mate=score.mate()
                    cp=10000 if mate>0 else -10000
                else:
                    cp=0
            cp=max(-1000,min(1000,cp))
            entry={
                "fen": board.fen(),
                "eval_cp": cp,
                "sf_eval_cp": cp,
                "depth": 12,
                "tag": "diverse_sf18",
                "result": "*"
            }
            out.write(json.dumps(entry)+"\n")
            count+=1
            if count%500==0:
                print(f"Generated {count} diverse SF18 positions")
        except Exception as e:
            print(f"Error {e}")
            continue

sf.quit()
print(f"Done {count} positions to {OUT}")
