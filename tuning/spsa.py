#!/usr/bin/env python3
"""
SPSA tuner for LMR, futility margins, history bonuses - Stockfish-style parameter tuning
- Perturbs parameters
- Runs self-play games
- Updates gradient
"""

import random, json, subprocess, math, argparse
from pathlib import Path

# Parameters to tune: LMR base, LMR div, futility margins, null move R, history bonus
DEFAULT_PARAMS = {
    "lmr_base": 0.75,
    "lmr_mult": 0.35,
    "futility_base": 80,
    "futility_mult": 150,
    "reverse_futility_base": 60,
    "reverse_futility_mult": 120,
    "null_R_base": 3,
    "history_bonus_base": 16,
}

def perturb(params, delta, ck=0.1):
    new_params = {}
    for k,v in params.items():
        # SPSA perturbation: add +/- ck*delta
        if random.choice([-1,1])>0:
            new_params[k] = v + ck*delta.get(k, 0.1)*random.uniform(0.5,1.5)
        else:
            new_params[k] = v - ck*delta.get(k, 0.1)*random.uniform(0.5,1.5)
    return new_params

def evaluate_params(engine_path, params, games=20):
    # Write params to file that engine reads? For demo we just simulate win rate based on closeness to optimal
    # In real SPSA, you'd recompile engine with new params and play vs baseline
    # Here we simulate with self-play using current engine vs baseline with slightly different params
    # For simplicity, return random Elo diff centered around 0 with small variance based on params distance
    # Real implementation would call cutechess-cli
    optimal = {"lmr_base":0.75,"lmr_mult":0.35,"futility_base":80,"futility_mult":150}
    dist = sum((params.get(k,0)-optimal.get(k,0))**2 for k in optimal)
    # Closer to optimal -> higher win rate
    win_rate = 0.5 + (10 - dist*2)*0.01
    win_rate = max(0.1, min(0.9, win_rate))
    # Simulate games
    wins = sum(1 for _ in range(games) if random.random() < win_rate)
    losses = games - wins
    # Elo diff approx
    if wins==0 or wins==games:
        elo=0
    else:
        p = wins/games
        elo = -400*math.log10(1/p -1)
    return elo, wins, losses

def spsa_loop(engine_path, iterations=20, games_per_iter=20):
    params = DEFAULT_PARAMS.copy()
    alpha=0.1
    ck=0.1
    print(f"Starting SPSA with {params}")
    for it in range(iterations):
        delta = {k: random.uniform(-1,1) for k in params}
        # Two evaluations: params + ck*delta and params - ck*delta
        params_plus = {k: params[k] + ck*delta[k] for k in params}
        params_minus = {k: params[k] - ck*delta[k] for k in params}

        elo_plus, w_plus, l_plus = evaluate_params(engine_path, params_plus, games_per_iter)
        elo_minus, w_minus, l_minus = evaluate_params(engine_path, params_minus, games_per_iter)

        # Gradient estimate
        grad = {}
        for k in params:
            grad[k] = (elo_plus - elo_minus) / (2*ck*delta[k] + 1e-6)

        # Update params: theta = theta + alpha*grad
        for k in params:
            params[k] += alpha * grad[k]
            # Clamp
            if "base" in k:
                params[k] = max(0.1, min(2.0, params[k]))
            if "mult" in k:
                params[k] = max(10, min(500, params[k]))

        print(f"Iter {it}: Elo+ {elo_plus:.2f} ({w_plus}-{l_plus}) Elo- {elo_minus:.2f} ({w_minus}-{l_minus}) Params {params}")
        # Save
        Path("tuning/params.json").write_text(json.dumps(params, indent=2))
        alpha *= 0.99
        ck *= 0.99

    print("Final params", params)
    return params

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("--engine", default="../build/chess_engine")
    ap.add_argument("--iter", type=int, default=10)
    ap.add_argument("--games", type=int, default=20)
    args=ap.parse_args()
    spsa_loop(args.engine, args.iter, args.games)
