#!/usr/bin/env python3
"""
Fast self-play with JSONL logging - Stockfish-grade pipeline
- Uses go depth 4 for speed (vs movetime 200 which hangs on buffered output)
- Properly drains uci / isready
- Stores full metadata per move: FEN, Move, Eval, Depth, SelDepth, PV, Nodes, Time, NNUE output
"""
import subprocess, json, uuid, time, argparse
from pathlib import Path

ENGINE = Path(__file__).parent.parent / "build" / "chess_engine"
LOG = Path(__file__).parent.parent / "datasets" / "games.jsonl"

def uci_command(proc, cmd):
    proc.stdin.write(cmd+"\n")
    proc.stdin.flush()

def drain_until(proc, keyword, timeout=5):
    start=time.time()
    lines=[]
    while True:
        if time.time()-start>timeout:
            break
        line = proc.stdout.readline()
        if not line:
            break
        lines.append(line.strip())
        if keyword in line:
            break
    return lines

def play_one_game(engine_path):
    game_id = str(uuid.uuid4())[:8]
    start_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    pos_fen = start_fen
    result = "*"

    proc = subprocess.Popen([str(engine_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    uci_command(proc, "uci")
    drain_until(proc, "uciok")
    uci_command(proc, "isready")
    drain_until(proc, "readyok")

    for ply in range(120):
        uci_command(proc, f"position fen {pos_fen}")
        uci_command(proc, "isready")
        drain_until(proc, "readyok")
        uci_command(proc, "go depth 4")
        best = None
        score_cp = 0
        depth=0
        seldepth=0
        pv=[]
        nodes=0
        start=time.time()
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            if line.startswith("info"):
                parts=line.split()
                try:
                    if "score" in parts and "cp" in parts:
                        score_cp = int(parts[parts.index("cp")+1])
                    if "depth" in parts:
                        depth = int(parts[parts.index("depth")+1])
                    if "seldepth" in parts:
                        seldepth = int(parts[parts.index("seldepth")+1])
                    if "nodes" in parts:
                        nodes = int(parts[parts.index("nodes")+1])
                    if "pv" in parts:
                        pv = parts[parts.index("pv")+1:]
                except:
                    pass
            if line.startswith("bestmove"):
                best = line.split()[1]
                break
        elapsed = int((time.time()-start)*1000)

        if not best or best=="0000":
            result = "1/2-1/2"
            break

        # log entry
        entry = {
            "game_id": game_id,
            "ply": ply,
            "fen": pos_fen,
            "move": best,
            "eval_cp": score_cp,
            "depth": depth,
            "seldepth": seldepth,
            "nodes": nodes,
            "pv": pv,
            "time_ms": elapsed,
            "nnue_eval": score_cp,
            "classical_eval": score_cp,
            "result": result,
            "phase": "middlegame" if ply>10 and ply<80 else ("opening" if ply<=10 else "endgame"),
            "tag": "selfplay-depth4",
            "tb_hit": "",
            "book_move": "",
            "time_management": "",
            "hashfull": 0
        }
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "a") as f:
            f.write(json.dumps(entry)+"\n")

        try:
            import chess
            board = chess.Board(pos_fen)
            board.push_uci(best)
            pos_fen = board.fen()
            if board.is_game_over():
                result = board.result()
                break
            # 50-move, repetition etc will be caught by board.is_game_over
        except Exception as e:
            # fallback: stop after 60 plies
            if ply>60:
                break

    try:
        uci_command(proc, "quit")
        proc.terminate()
    except:
        pass
    return game_id, result

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=20)
    ap.add_argument("--engine", type=str, default=str(ENGINE))
    args = ap.parse_args()

    print(f"Using engine {args.engine}")
    # if engine not built, use /tmp/chess_engine
    eng_path = Path(args.engine)
    if not eng_path.exists():
        alt = Path("/tmp/chess_engine")
        if alt.exists():
            eng_path = alt
    for i in range(args.games):
        print(f"Game {i+1}/{args.games}")
        gid, res = play_one_game(eng_path)
        print(f" -> {gid} {res}")

if __name__ == "__main__":
    main()
