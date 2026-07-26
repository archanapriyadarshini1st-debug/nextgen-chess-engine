#!/usr/bin/env python3
"""Match harness: NextGen engine vs Stockfish 18. Logs PGN + per-ply JSONL."""
import chess, chess.pgn, chess.engine, json, time, os, random, argparse, datetime, traceback

ENG = os.environ.get('NG_ENGINE', '/home/user/webapp/engine/bin/chess_engine')
SF  = os.environ.get('SF_BIN', '/home/user/sf/stockfish18')
LAB = '/home/user/webapp/lab'

OPENINGS = {
 "startpos": [],
 "Italian": ["e2e4","e7e5","g1f3","b8c6","f1c4"],
 "Ruy Lopez": ["e2e4","e7e5","g1f3","b8c6","f1b5"],
 "Sicilian Najdorf": ["e2e4","c7c5","g1f3","d7d6","d2d4","c5d4","f3d4","g8f6","b1c3","a7a6"],
 "French": ["e2e4","e7e6","d2d4","d7d5"],
 "Caro-Kann": ["e2e4","c7c6","d2d4","d7d5"],
 "QGD": ["d2d4","d7d5","c2c4","e7e6"],
 "Slav": ["d2d4","d7d5","c2c4","c7c6"],
 "KID": ["d2d4","g8f6","c2c4","g7g6","b1c3","f8g7"],
 "Nimzo-Indian": ["d2d4","g8f6","c2c4","e7e6","b1c3","f8b4"],
 "English": ["c2c4","e7e5"],
 "London": ["d2d4","d7d5","c1f4"],
 "Scandinavian": ["e2e4","d7d5"],
 "Pirc": ["e2e4","d7d6","d2d4","g8f6"],
 "Dutch": ["d2d4","f7f5"],
 "Benoni": ["d2d4","g8f6","c2c4","c7c5","d4d5"],
}

def open_engines(sf_elo=None, sf_depth=None, hash_mb=64, threads=1):
    ng = chess.engine.SimpleEngine.popen_uci(ENG, timeout=30)
    sf = chess.engine.SimpleEngine.popen_uci(SF, timeout=30)
    try: ng.configure({"Hash": hash_mb, "Threads": threads})
    except Exception: pass
    cfg = {"Hash": hash_mb, "Threads": threads}
    if sf_elo is not None:
        cfg.update({"UCI_LimitStrength": True, "UCI_Elo": int(sf_elo)})
    try: sf.configure(cfg)
    except Exception:
        try: sf.configure({"Hash": hash_mb})
        except Exception: pass
    return ng, sf

def play_game(ng, sf, gid, ng_white, opening_name, opening_moves, tc, sf_elo, sf_depth):
    board = chess.Board()
    for u in opening_moves:
        m = chess.Move.from_uci(u)
        if m in board.legal_moves: board.push(m)
    game = chess.pgn.Game()
    game.headers["Event"] = "NextGen vs SF18 improvement loop"
    game.headers["Date"] = datetime.datetime.now().strftime("%Y.%m.%d")
    game.headers["White"] = "NextGen" if ng_white else f"Stockfish18({sf_elo or 'full'})"
    game.headers["Black"] = f"Stockfish18({sf_elo or 'full'})" if ng_white else "NextGen"
    game.headers["Opening"] = opening_name
    game.headers["TimeControl"] = str(tc)
    game.setup(chess.Board())
    node = game
    for u in opening_moves: node = node.add_variation(chess.Move.from_uci(u))

    plies = []
    limit_ng = chess.engine.Limit(time=tc)
    limit_sf = chess.engine.Limit(depth=sf_depth) if sf_depth else chess.engine.Limit(time=tc)
    term = "normal"
    while not board.is_game_over(claim_draw=True) and board.fullmove_number < 160:
        ours = (board.turn == chess.WHITE) == ng_white
        eng  = ng if ours else sf
        lim  = limit_ng if ours else limit_sf
        t0 = time.time()
        try:
            res = eng.play(board, lim, info=chess.engine.INFO_ALL)
        except Exception as e:
            term = f"crash:{'ng' if ours else 'sf'}:{type(e).__name__}"
            break
        el = time.time() - t0
        mv = res.move
        if mv is None or mv not in board.legal_moves:
            term = f"illegal:{'ng' if ours else 'sf'}:{mv}"
            break
        info = res.info or {}
        sc = info.get("score")
        cp = None; mate = None
        if sc is not None:
            pov = sc.pov(board.turn)
            cp = pov.score(); mate = pov.mate()
        plies.append({
            "game_id": gid, "ply": board.ply(), "fen": board.fen(),
            "mover": "ng" if ours else "sf", "move": mv.uci(),
            "san": board.san(mv),
            "cp": cp, "mate": mate, "depth": info.get("depth"),
            "seldepth": info.get("seldepth"), "nodes": info.get("nodes"),
            "nps": info.get("nps"), "time_s": round(el,4),
            "pv": [m.uci() for m in (info.get("pv") or [])][:8],
            "is_capture": board.is_capture(mv), "gives_check": board.gives_check(mv),
            "piece": board.piece_at(mv.from_square).symbol() if board.piece_at(mv.from_square) else None,
        })
        board.push(mv); node = node.add_variation(mv)

    result = board.result(claim_draw=True) if term == "normal" else "*"
    if term != "normal" and term.startswith(("crash","illegal")):
        loser_ng = ":ng:" in term
        result = "0-1" if (loser_ng == ng_white) else "1-0"
    game.headers["Result"] = result
    game.headers["Termination"] = term
    if result == "1-0": ng_score = 1.0 if ng_white else 0.0
    elif result == "0-1": ng_score = 0.0 if ng_white else 1.0
    else: ng_score = 0.5
    return game, plies, ng_score, result, term, board

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=20)
    ap.add_argument("--sf-elo", type=int, default=1600)
    ap.add_argument("--sf-depth", type=int, default=None)
    ap.add_argument("--tc", type=float, default=0.25)
    ap.add_argument("--tag", default="baseline")
    ap.add_argument("--engine", default=None)
    args = ap.parse_args()
    global ENG
    if args.engine: ENG = args.engine

    os.makedirs(f"{LAB}/pgn", exist_ok=True); os.makedirs(f"{LAB}/data", exist_ok=True)
    pgn_path = f"{LAB}/pgn/{args.tag}.pgn"
    jsl_path = f"{LAB}/data/{args.tag}_plies.jsonl"
    sum_path = f"{LAB}/data/{args.tag}_games.jsonl"

    ng, sf = open_engines(sf_elo=args.sf_elo)
    W=L=D=0; names=list(OPENINGS)
    with open(pgn_path,"a") as pf, open(jsl_path,"a") as jf, open(sum_path,"a") as sf_:
        for g in range(args.games):
            on = names[g % len(names)]
            try:
                game, plies, score, result, term, board = play_game(
                    ng, sf, g, g%2==0, on, OPENINGS[on], args.tc, args.sf_elo, args.sf_depth)
            except Exception:
                traceback.print_exc()
                try: ng.quit()
                except Exception: pass
                try: sf.quit()
                except Exception: pass
                ng, sf = open_engines(sf_elo=args.sf_elo); continue
            if score==1.0: W+=1
            elif score==0.0: L+=1
            else: D+=1
            print(game, file=pf, end="\n\n"); pf.flush()
            for p in plies: jf.write(json.dumps(p)+"\n")
            jf.flush()
            sf_.write(json.dumps({"game_id":g,"tag":args.tag,"opening":on,
                "ng_white":g%2==0,"result":result,"ng_score":score,"term":term,
                "sf_elo":args.sf_elo,"tc":args.tc,"plies":len(plies),
                "final_fen":board.fen()})+"\n"); sf_.flush()
            print(f"[{args.tag}] game {g+1}/{args.games} {on:16s} {result:7s} {term:12s} W{W} L{L} D{D}", flush=True)
    try: ng.quit()
    except Exception: pass
    try: sf.quit()
    except Exception: pass
    n=W+L+D
    print(f"RESULT tag={args.tag} sf_elo={args.sf_elo} W={W} L={L} D={D} score={(W+0.5*D)/max(n,1):.3f}")

if __name__ == "__main__":
    main()
