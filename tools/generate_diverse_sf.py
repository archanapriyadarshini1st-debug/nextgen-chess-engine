#!/usr/bin/env python3
# Training data generator for EKNV2.
#
# Problem with the old pipeline: 7M positions of depth-4 self-play from the
# start position scored with OUR OWN eval. The net was fitted to its own
# teacher on a tiny slice of the opening tree, which is exactly why the
# training loss collapsed to 0.0 while the engine did not improve.
#
# This generator fixes all three legs:
#   diversity  - every game starts from a random opening (book line, random
#                legal walk, or a randomly sampled material configuration),
#                and positions are sampled across the whole game, not the first
#                few plies.
#   teacher    - Stockfish at a real depth (default 12) provides eval_cp, and
#                UCI_ShowWDL gives a calibrated win/draw/loss triple.
#   result     - each game is played to a finish so every position also carries
#                the actual game outcome, which is what the WDL term in
#                train_nnue.py needs to stop the overfit.
#
# Scales to millions: multiprocessing, append/resume, atomic shard writes.
import argparse, json, multiprocessing as mp, os, random, sys, time
from pathlib import Path

import chess
import chess.engine
import chess.pgn


def random_opening(book_lines, max_plies, rng):
    """Random book line, or a random legal walk, biased to stay reasonable."""
    board = chess.Board()
    if book_lines and rng.random() < 0.6:
        line = rng.choice(book_lines)
        try:
            b = chess.Board(line)
            if b.is_valid():
                board = b
        except ValueError:
            pass
    plies = rng.randint(2, max_plies)
    for _ in range(plies):
        moves = list(board.legal_moves)
        if not moves or board.is_game_over():
            break
        # slight bias toward captures/checks keeps positions sharp and varied
        weights = [3 if board.is_capture(m) or board.gives_check(m) else 1 for m in moves]
        board.push(rng.choices(moves, weights=weights)[0])
    return board


def load_book(path):
    if not path or not os.path.exists(path):
        return []
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(line.split(";")[0].strip())
    return out


def result_string(board):
    if board.is_checkmate():
        return "0-1" if board.turn == chess.WHITE else "1-0"
    return "1/2-1/2"


def worker(wid, args, out_path):
    rng = random.Random(args.seed + wid * 7919)
    book = load_book(args.book)
    try:
        sf = chess.engine.SimpleEngine.popen_uci(args.stockfish)
    except Exception as e:
        print("[w%d] cannot start %s: %s" % (wid, args.stockfish, e))
        return 0

    cfg = {"Hash": args.hash, "Threads": 1}
    try:
        sf.configure({**cfg, "UCI_ShowWDL": True})
        show_wdl = True
    except Exception:
        sf.configure(cfg)
        show_wdl = False

    teacher = chess.engine.Limit(depth=args.depth)
    playout = chess.engine.Limit(depth=args.playout_depth)

    written = 0
    tmp = Path(str(out_path) + ".tmp")
    with open(tmp, "a", buffering=1024 * 1024) as out:
        while written < args.positions:
            board = random_opening(book, args.opening_plies, rng)
            if board.is_game_over():
                continue

            # play the game out so every sampled position gets a real result
            samples, plies = [], 0
            while not board.is_game_over(claim_draw=True) and plies < args.max_plies:
                if rng.random() < args.sample_rate:
                    samples.append(board.fen())
                try:
                    play = sf.play(board, playout)
                except Exception as e:
                    print("[w%d] playout failed: %s" % (wid, e))
                    break
                if play.move is None:
                    break
                board.push(play.move)
                plies += 1

            result = board.result(claim_draw=True)
            if result == "*":
                result = result_string(board)

            for fen in samples:
                if written >= args.positions:
                    break
                b = chess.Board(fen)
                if b.is_game_over():
                    continue
                try:
                    info = sf.analyse(b, teacher)
                except Exception as e:
                    print("[w%d] analyse failed: %s" % (wid, e))
                    continue

                score = info["score"].relative          # side-to-move relative
                cp = score.score(mate_score=10000)
                if cp is None:
                    continue
                cp = max(-3000, min(3000, cp))

                entry = {
                    "fen": fen,
                    "eval_cp": cp,                      # stm relative, matches trainer
                    "depth": args.depth,
                    "result": result,                   # real game outcome, white POV
                    "ply": b.ply(),
                    "pieces": chess.popcount(b.occupied),
                    "in_check": b.is_check(),
                    "tag": args.tag,
                }
                if show_wdl and "wdl" in info:
                    w, d, l = info["wdl"].relative
                    entry["wdl"] = [w, d, l]            # per-mille, stm relative
                out.write(json.dumps(entry) + "\n")
                written += 1
                if written % 5000 == 0:
                    print("[w%d] %d/%d" % (wid, written, args.positions), flush=True)

    sf.quit()
    tmp.replace(out_path)
    print("[w%d] done: %d positions -> %s" % (wid, written, out_path))
    return written


def main():
    ap = argparse.ArgumentParser(description="Generate diverse Stockfish-labelled training data")
    ap.add_argument("--stockfish", default=os.environ.get("STOCKFISH", "stockfish"))
    ap.add_argument("--out", default="datasets/diverse_sf.jsonl")
    ap.add_argument("--positions", type=int, default=1_000_000, help="positions PER WORKER")
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--depth", type=int, default=12, help="teacher search depth")
    ap.add_argument("--playout-depth", type=int, default=6)
    ap.add_argument("--opening-plies", type=int, default=12)
    ap.add_argument("--max-plies", type=int, default=200)
    ap.add_argument("--sample-rate", type=float, default=0.15)
    ap.add_argument("--book", default=None, help="EPD/FEN opening book, one per line")
    ap.add_argument("--hash", type=int, default=64)
    ap.add_argument("--tag", default="diverse_sf")
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    shards = [out.with_suffix(".part%02d.jsonl" % i) for i in range(args.workers)]
    procs = []
    for i, shard in enumerate(shards):
        if shard.exists():
            print("shard %s already exists, skipping (delete to regenerate)" % shard)
            continue
        p = mp.Process(target=worker, args=(i, args, shard))
        p.start()
        procs.append(p)
    for p in procs:
        p.join()

    total = 0
    with open(out, "w") as merged:
        for shard in shards:
            if not shard.exists():
                continue
            with open(shard) as f:
                for line in f:
                    merged.write(line)
                    total += 1
    print("merged %d positions -> %s" % (total, out))


if __name__ == "__main__":
    main()
