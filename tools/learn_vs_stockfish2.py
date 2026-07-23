#!/usr/bin/env python3
"""Learn vs Stockfish - 10000 games with why/when"""
import chess, chess.engine, json, random, time
from pathlib import Path

STOCKFISH="bin/stockfish18"
OUR="bin/chess_engine"
OUT="datasets/learn_vs_sf18.jsonl"

def explain(board, move):
    # Why best?
    piece=board.piece_at(move.from_square)
    captured=board.piece_at(move.to_square)
    is_cap = captured is not None or board.is_en_passant(move)
    board.push(move)
    is_check = board.is_check()
    board.pop()
    total=len(board.piece_map())
    phase="opening" if board.ply()<20 else ("endgame" if total<10 else "middlegame")
    why=[]
    if is_cap:
        why.append(f"wins {captured.symbol() if captured else 'material'}")
    if is_check:
        why.append("gives check")
    if not why:
        why.append("positional")
    return phase, ", ".join(why)

Path(OUT).parent.mkdir(parents=True, exist_ok=True)

games=200
count=0
for game_idx in range(games):
    # Start fresh engines per game to avoid crash propagation
    try:
        sf=chess.engine.SimpleEngine.popen_uci(STOCKFISH)
        our=chess.engine.SimpleEngine.popen_uci(OUR)
        sf.configure({"Hash":32,"Threads":1})
        our.configure({"Hash":64,"Threads":1})
    except Exception as e:
        print(f"Failed to start engines {e}")
        time.sleep(1)
        continue

    board=chess.Board()
    # Random opening 4-8 moves
    for _ in range(random.randint(4,8)):
        if board.is_game_over():
            break
        moves=list(board.legal_moves)
        if not moves:
            break
        board.push(random.choice(moves))

    our_white=(game_idx%2==0)
    try:
        for ply in range(120):
            if board.is_game_over():
                break
            # Get SF best for training label
            try:
                info=sf.analyse(board, chess.engine.Limit(depth=12))
                sf_score=info["score"].relative
                cp=sf_score.score(mate_score=10000)
                if cp is None:
                    cp=10000 if sf_score.mate() and sf_score.mate()>0 else -10000
                cp=max(-1000,min(1000,cp))
                pv=info.get("pv",[])
                sf_best=pv[0] if pv else None
                phase,why=explain(board, sf_best if sf_best else chess.Move.null(), sf_score)
            except Exception as e:
                print(f"SF analyse error {e}")
                cp=0
                sf_best=None
                phase="middlegame"
                why="unknown"

            # Our or SF move?
            is_our = (board.turn==chess.WHITE and our_white) or (board.turn==chess.BLACK and not our_white)
            engine = our if is_our else sf
            try:
                res=engine.play(board, chess.engine.Limit(time=0.3))
                if res.move is None:
                    break
                # Log training entry: FEN, SF best, SF eval, phase, why, when
                entry={
                    "game_id":game_idx,
                    "ply":board.ply(),
                    "fen":board.fen(),
                    "our_move":res.move.uci(),
                    "sf_best":sf_best.uci() if sf_best else None,
                    "sf_eval_cp":cp,
                    "phase":phase,
                    "why_best":why,
                    "when_to_play":phase,
                    "matches_sf": (res.move==sf_best) if res.move and sf_best else False,
                    "is_capture": board.is_capture(res.move),
                    "result":"*"
                }
                with open(OUT,"a") as f:
                    f.write(json.dumps(entry)+"\n")
                count+=1
                board.push(res.move)
            except Exception as e:
                print(f"Play error game {game_idx} ply {ply}: {e}")
                break

        print(f"Game {game_idx+1}/{games} finished {board.result()} in {board.ply()} plies, total logged {count}")
    finally:
        try: sf.quit()
        except: pass
        try: our.quit()
        except: pass
        time.sleep(0.3)

print(f"Done, logged {count} positions to {OUT}")
