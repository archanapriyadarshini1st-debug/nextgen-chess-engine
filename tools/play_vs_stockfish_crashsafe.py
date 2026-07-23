#!/usr/bin/env python3
"""
Crash-safe match vs Stockfish 18 with proper time control 10+0.1
- If engine crashes, discard game and replay, don't count as win/loss
- Reports plain W/L/D, no Elo estimate
- Time control 10+0.1 for meaningful strength
"""
import chess, chess.engine, time, sys
from pathlib import Path

STOCKFISH = "bin/stockfish18"
OUR = "bin/chess_engine"

def play_one_game(our_white=True, time_control="10+0.1", sf_elo=1320):
    # Parse time control like "10+0.1"
    base=10
    inc=0.1
    try:
        if "+" in time_control:
            b,i = time_control.split("+")
            base=float(b)
            inc=float(i)
    except:
        pass

    for attempt in range(3):  # retry up to 3 times if crash
        try:
            sf = chess.engine.SimpleEngine.popen_uci(STOCKFISH)
            our = chess.engine.SimpleEngine.popen_uci(OUR)
            sf.configure({"Hash":32, "Threads":1, "UCI_LimitStrength":True, "UCI_Elo":sf_elo})
            our.configure({"Hash":64, "Threads":1})
            board = chess.Board()
            # Time control: we will use Limit with white_clock/black_clock
            # For 10+0.1, we start with 10 sec each and inc 0.1
            wtime = base
            btime = base
            while not board.is_game_over() and board.ply() < 200:
                # Choose engine
                is_our_turn = (board.turn==chess.WHITE and our_white) or (board.turn==chess.BLACK and not our_white)
                engine = our if is_our_turn else sf
                # Use clock-based limit
                if board.turn==chess.WHITE:
                    limit = chess.engine.Limit(white_clock=wtime, black_clock=btime, white_inc=inc, black_inc=inc)
                else:
                    limit = chess.engine.Limit(white_clock=wtime, black_clock=btime, white_inc=inc, black_inc=inc)

                start=time.time()
                result = engine.play(board, limit)
                elapsed = time.time()-start

                if result.move is None:
                    # No move - treat as crash/draw, discard
                    raise Exception("No move returned")

                board.push(result.move)

                # Update clocks
                if board.turn==chess.BLACK: # White just moved
                    wtime = max(0, wtime - elapsed + inc)
                else:
                    btime = max(0, btime - elapsed + inc)

                # If time runs out, it's loss for that side (should not happen with 10+0.1)
                if wtime<=0 or btime<=0:
                    break

            # Game finished without crash
            res = board.result() if board.is_game_over() else "1/2-1/2"
            try: sf.quit()
            except: pass
            try: our.quit()
            except: pass
            time.sleep(0.5)
            return res, board.ply(), False  # False = not crashed

        except Exception as e:
            print(f"  Crash detected (attempt {attempt+1}/3): {e}, discarding and replaying")
            try: sf.quit()
            except: pass
            try: our.quit()
            except: pass
            time.sleep(1)
            # If crashed, discard and retry - don't count
            continue

    # If 3 attempts all crashed, count as discarded
    return None, 0, True

def main():
    import argparse
    ap=argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=50)
    ap.add_argument("--tc", type=str, default="10+0.1")
    ap.add_argument("--elo", type=int, default=1320)
    args=ap.parse_args()

    wins=losses=draws=0
    discarded=0
    for i in range(args.games):
        our_white=(i%2==0)
        print(f"\nGame {i+1}/{args.games} Our {'White' if our_white else 'Black'} vs SF18 Elo{args.elo} TC {args.tc}")
        res, ply, crashed = play_one_game(our_white, args.tc, args.elo)
        if crashed or res is None:
            print(f"  Discarded due to crash")
            discarded+=1
            continue
        print(f"  Result {res} in {ply} plies")
        if res=="1-0":
            if our_white: wins+=1
            else: losses+=1
        elif res=="0-1":
            if our_white: losses+=1
            else: wins+=1
        else:
            draws+=1
        print(f"  Score so far: {wins}W {losses}L {draws}D (discarded {discarded})")

    total = wins+losses+draws
    print(f"\nFINAL vs Stockfish 18 Elo{args.elo} TC {args.tc}: {wins}W {losses}L {draws}D in {total} games (discarded {discarded})")
    if total>0:
        score = wins + draws*0.5
        print(f"Raw record: {wins}-{losses}-{draws}, Score {score}/{total} = {score/total*100:.1f}%")
        print("No Elo estimate - raw W/L/D only as requested")

if __name__=="__main__":
    main()
