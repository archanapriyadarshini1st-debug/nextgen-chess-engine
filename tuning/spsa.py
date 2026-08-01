#!/usr/bin/env python3
# Real SPSA tuner for the search parameters exposed as UCI spin options.
#
# The previous version simulated a win rate from the euclidean distance to a
# hardcoded optimal dict, so it always converged to that dict and never touched
# the engine. This one plays actual games with cutechess-cli, uses the standard
# Spall gain schedules, checkpoints every iteration so a crash is resumable,
# and finishes with an SPRT of tuned-vs-baseline that REVERTS on no gain.
import argparse, json, math, os, random, shutil, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from sprt import EngineSpec, elo_with_error, play_match, run_sprt  # noqa: E402

# name -> (default, min, max, typical SPSA step)
PARAMS = {
    "LmrBase":      (75,  20,  200, 8),
    "LmrMult":      (35,  10,  100, 5),
    "FutilityBase": (80,  20,  300, 15),
    "FutilityMult": (150, 40,  400, 25),
    "RfpBase":      (60,  10,  250, 12),
    "RfpMult":      (120, 30,  350, 20),
    "NullRBase":    (3,   1,   6,   1),
    "HistoryBonus": (16,  4,   64,  4),
    "Aspiration":   (50,  10,  200, 10),
}

CHECKPOINT = Path("tuning/spsa_state.json")
BEST = Path("tuning/params.json")


def clamp(name, value):
    _, lo, hi = PARAMS[name][0], PARAMS[name][1], PARAMS[name][2]
    return int(round(min(max(value, lo), hi)))


def load_state(defaults):
    if CHECKPOINT.exists():
        try:
            s = json.loads(CHECKPOINT.read_text())
            print("resuming from iteration %d" % s["iteration"])
            return s
        except Exception as e:
            print("checkpoint unreadable (%s), starting fresh" % e)
    return {"iteration": 0, "theta": dict(defaults), "history": []}


def save_state(state):
    CHECKPOINT.parent.mkdir(parents=True, exist_ok=True)
    tmp = CHECKPOINT.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2))
    tmp.replace(CHECKPOINT)   # atomic: a crash mid-write cannot corrupt it


def spsa(args):
    if not os.path.exists(args.engine):
        sys.exit("engine not found: %s" % args.engine)

    defaults = {k: v[0] for k, v in PARAMS.items()}
    baseline = dict(defaults)
    state = load_state(defaults)
    theta = {k: float(v) for k, v in state["theta"].items()}

    # Spall gain sequences
    A = 0.1 * args.iterations
    alpha, gamma = 0.602, 0.101
    a = args.a
    c = args.c

    for k in range(state["iteration"], args.iterations):
        ak = a / (k + 1 + A) ** alpha
        ck = c / (k + 1) ** gamma

        delta = {p: random.choice([-1.0, 1.0]) for p in PARAMS}
        step = {p: ck * PARAMS[p][3] * delta[p] for p in PARAMS}

        plus = {p: clamp(p, theta[p] + step[p]) for p in PARAMS}
        minus = {p: clamp(p, theta[p] - step[p]) for p in PARAMS}

        if plus == minus:
            continue  # step rounded away, nothing to measure

        t0 = time.time()
        res = play_match(
            EngineSpec(args.engine, "plus", dict(plus)),
            EngineSpec(args.engine, "minus", dict(minus)),
            games=args.games, tc=args.tc, concurrency=args.concurrency,
            openings=args.openings, hash_mb=args.hash, threads=args.threads,
            pgnout=args.pgnout,
        )

        if res.games < args.games * 0.5:
            # engine crashed or cutechess died: do not poison theta with a
            # half-finished sample, just retry the iteration next time.
            print("iter %3d: only %d/%d games completed, skipping update"
                  % (k, res.games, args.games))
            print(res.raw[-500:])
            continue

        # gradient estimate: score difference in [-0.5, 0.5]
        y = res.score - 0.5
        grad = {p: y / (2.0 * step[p]) if step[p] else 0.0 for p in PARAMS}
        for p in PARAMS:
            theta[p] = clamp(p, theta[p] + ak * grad[p] * PARAMS[p][3] ** 2)

        elo, err = elo_with_error(res.wins, res.losses, res.draws)
        print("iter %3d  %s  ck=%.3f ak=%.4f  %.0fs"
              % (k, res, ck, ak, time.time() - t0))
        print("          theta=%s" % json.dumps({p: int(theta[p]) for p in PARAMS}))

        state["iteration"] = k + 1
        state["theta"] = {p: int(theta[p]) for p in PARAMS}
        state["history"].append({"iter": k, "w": res.wins, "l": res.losses,
                                 "d": res.draws, "elo": elo, "err": err,
                                 "crashes": res.crashes})
        save_state(state)

    tuned = {p: int(theta[p]) for p in PARAMS}
    print("\ntuned params: %s" % json.dumps(tuned, indent=2))

    if args.no_verify:
        BEST.write_text(json.dumps(tuned, indent=2))
        return tuned

    print("\nSPRT tuned vs baseline (elo0=%.1f elo1=%.1f)..." % (args.elo0, args.elo1))
    verdict, total = run_sprt(
        EngineSpec(args.engine, "tuned", dict(tuned)),
        EngineSpec(args.engine, "base", dict(baseline)),
        elo0=args.elo0, elo1=args.elo1,
        batch=args.verify_batch, max_games=args.verify_max,
        tc=args.tc, concurrency=args.concurrency, openings=args.openings,
        hash_mb=args.hash, threads=args.threads,
    )
    print("SPRT verdict: %s  %s" % (verdict, total))

    if verdict == "accept":
        BEST.parent.mkdir(parents=True, exist_ok=True)
        BEST.write_text(json.dumps(tuned, indent=2))
        print("accepted - wrote %s" % BEST)
        return tuned

    print("NOT accepted - reverting to baseline (no regression shipped)")
    BEST.write_text(json.dumps(baseline, indent=2))
    return baseline


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="SPSA tune search params over real games")
    ap.add_argument("--engine", default="build/chess_engine")
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--games", type=int, default=200, help="games per SPSA iteration")
    ap.add_argument("--tc", default="10+0.1")
    ap.add_argument("--concurrency", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--openings", default=None, help="epd/pgn opening book")
    ap.add_argument("--hash", type=int, default=64)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--pgnout", default=None)
    ap.add_argument("--a", type=float, default=0.15)
    ap.add_argument("--c", type=float, default=1.0)
    ap.add_argument("--elo0", type=float, default=0.0)
    ap.add_argument("--elo1", type=float, default=4.0)
    ap.add_argument("--verify-batch", type=int, default=200)
    ap.add_argument("--verify-max", type=int, default=20000)
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    random.seed(args.seed)
    spsa(args)
