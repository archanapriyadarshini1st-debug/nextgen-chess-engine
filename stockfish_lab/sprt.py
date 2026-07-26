#!/usr/bin/env python3
"""SPRT / statistical gating: only keep changes that are significant improvements."""
import math

def elo(score):
    if score <= 0: return -800.0
    if score >= 1: return 800.0
    return -400.0 * math.log10(1.0/score - 1.0)

def bayeselo_ll(w, l, d, elo_):
    """Log-likelihood of W/L/D under a given Elo, using the 5-nomial-free BayesElo model."""
    n = w + l + d
    if n == 0: return 0.0
    s = (w + 0.5*d) / n
    return s

def sprt(w, l, d, elo0=0.0, elo1=10.0, alpha=0.05, beta=0.05):
    """Sequential Probability Ratio Test.
    H0: engine gained elo0. H1: engine gained elo1.
    Returns (llr, lower_bound, upper_bound, decision)."""
    n = w + l + d
    if n == 0 or (w + l) == 0:
        return 0.0, math.log(beta/(1-alpha)), math.log((1-beta)/alpha), "continue"

    def p_from_elo(e):
        return 1.0 / (1.0 + 10 ** (-e/400.0))

    # drawelo-based model
    s0, s1 = p_from_elo(elo0), p_from_elo(elo1)
    # probabilities of win/loss given score and draw ratio
    dr = d / n
    def wl(s):
        pw = s - dr/2.0
        pl = 1.0 - s - dr/2.0
        pw = min(max(pw, 1e-6), 1-1e-6)
        pl = min(max(pl, 1e-6), 1-1e-6)
        return pw, pl
    pw0, pl0 = wl(s0)
    pw1, pl1 = wl(s1)
    llr = w*math.log(pw1/pw0) + l*math.log(pl1/pl0)

    lower = math.log(beta/(1-alpha))
    upper = math.log((1-beta)/alpha)
    if llr >= upper:   dec = "ACCEPT (H1: improvement)"
    elif llr <= lower: dec = "REJECT (H0: no improvement)"
    else:              dec = "continue"
    return llr, lower, upper, dec

def score_ci(w, l, d, z=1.96):
    """Wilson-ish CI on the score rate, plus Elo bounds."""
    n = w + l + d
    if n == 0: return (0, 0, 0)
    s = (w + 0.5*d)/n
    var = (w*(1-s)**2 + d*(0.5-s)**2 + l*(0-s)**2) / n
    se = math.sqrt(var/n) if n else 0
    return s, s - z*se, s + z*se

def significant_gain(w, l, d, base_score, z=1.96):
    """True only if the new score's CI lower bound beats the baseline score."""
    s, lo, hi = score_ci(w, l, d, z)
    return (lo > base_score), s, lo, hi

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--wins", type=int, required=True)
    p.add_argument("--losses", type=int, required=True)
    p.add_argument("--draws", type=int, required=True)
    p.add_argument("--elo0", type=float, default=0.0)
    p.add_argument("--elo1", type=float, default=10.0)
    a = p.parse_args()
    n = a.wins + a.losses + a.draws
    s, lo, hi = score_ci(a.wins, a.losses, a.draws)
    llr, L, U, dec = sprt(a.wins, a.losses, a.draws, a.elo0, a.elo1)
    print(f"games={n} W={a.wins} L={a.losses} D={a.draws}")
    print(f"score={s:.4f} 95%CI=[{lo:.4f},{hi:.4f}]  Elo={elo(s):+.1f}")
    print(f"LLR={llr:.3f} bounds=[{L:.3f},{U:.3f}] -> {dec}")
