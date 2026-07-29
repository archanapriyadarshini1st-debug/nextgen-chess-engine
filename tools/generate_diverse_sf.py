#!/usr/bin/env python3
import argparse, chess, chess.engine, json, random
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--stockfish',default='bin/stockfish18'); ap.add_argument('--out',default='datasets/diverse_sf18.jsonl'); ap.add_argument('--positions',type=int,default=1000000); ap.add_argument('--depth',type=int,default=12); ap.add_argument('--opening-plies',type=int,default=16); ap.add_argument('--hash',type=int,default=256); a=ap.parse_args()
    sf=chess.engine.SimpleEngine.popen_uci(a.stockfish); sf.configure({'Hash':a.hash,'Threads':1}); Path(a.out).parent.mkdir(parents=True,exist_ok=True); count=0
    try:
        with open(a.out,'w') as out:
            while count<a.positions:
                b=chess.Board(); plies=random.randint(0,a.opening_plies)
                for _ in range(plies):
                    moves=list(b.legal_moves)
                    if not moves: break
                    b.push(random.choice(moves))
                if b.is_game_over(): continue
                info=sf.analyse(b,chess.engine.Limit(depth=a.depth)); rel=info['score'].pov(b.turn); cp=rel.score(mate_score=10000) or 0; cp=max(-1000,min(1000,cp))
                entry={'fen':b.fen(),'eval_cp':cp,'sf_eval_cp':cp,'depth':a.depth,'result':'*','wdl':rel.wdl().__dict__ if hasattr(rel.wdl(),'__dict__') else str(rel.wdl()),'threat_inputs':[]}
                out.write(json.dumps(entry)+'\n'); count+=1
                if count%10000==0: print(f'generated {count}/{a.positions}')
    finally: sf.quit()
if __name__=='__main__': main()
