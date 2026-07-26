#!/usr/bin/env python3
"""Continuous improvement loop vs Stockfish 18.

Cycle:
  1. play games vs SF18 (varied openings / strengths / time controls)
  2. save PGN + per-ply JSONL (eval, depth, nodes, nps, pv)
  3. analyse losses/draws/missed chances -> blunder + phase report
  4. mine recurring patterns (SF tendencies, our weak phases/pieces)
  5. propose a candidate change (a "variant" = a rebuilt binary)
  6. A/B the candidate vs the current champion under identical conditions
  7. keep it ONLY if the gain is statistically significant, else revert
  8. repeat; when gains plateau, widen the idea pool

Never copies Stockfish source: learns purely from game outcomes + PGNs.
"""
import argparse, json, os, subprocess, sys, time, shutil, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sprt import score_ci, elo, sprt

LAB = '/home/user/webapp/lab'
ENGDIR = '/home/user/webapp/engine'
STATE = f'{LAB}/state.json'

BUILD = ("g++ -std=c++20 -I. core/*.cpp search/*.cpp evaluation/*.cpp nnue/*.cpp "
         "tb/syzygy.cpp opening/*.cpp logging/*.cpp uci/*.cpp main.cpp "
         "-o bin/{out} -pthread -O2")

def sh(cmd, cwd=None, timeout=900):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True,
                          text=True, timeout=timeout)

def load_state():
    if os.path.exists(STATE):
        return json.load(open(STATE))
    return {'champion': 'chess_engine', 'cycle': 0, 'history': [], 'rejected': []}

def save_state(s):
    json.dump(s, open(STATE, 'w'), indent=1)

def build(out):
    r = sh(BUILD.format(out=out), cwd=ENGDIR, timeout=900)
    ok = os.path.exists(f'{ENGDIR}/bin/{out}')
    return ok, (r.stderr or '')[-2000:]

def play(tag, engine, games, sf_elo, tc):
    cmd = (f'python3 {LAB}/harness.py --games {games} --sf-elo {sf_elo} '
           f'--tc {tc} --tag {tag} --engine {engine}')
    r = sh(cmd, cwd=LAB, timeout=7200)
    W = L = D = 0
    for line in (r.stdout or '').splitlines():
        if line.startswith('RESULT'):
            for part in line.split():
                if part.startswith('W='): W = int(part[2:])
                if part.startswith('L='): L = int(part[2:])
                if part.startswith('D='): D = int(part[2:])
    return W, L, D, (r.stdout or '')[-1500:]

def analyse(tag, depth=10, maxg=16):
    r = sh(f'python3 {LAB}/analyze.py --tag {tag} --depth {depth} --max-games {maxg}',
           cwd=LAB, timeout=3600)
    return (r.stdout or '')[-2500:]

def gate(cand, base, min_games=16):
    """Keep candidate only on a statistically significant improvement."""
    cw, cl, cd = cand; bw, bl, bd = base
    cs, clo, chi = score_ci(cw, cl, cd)
    bs, blo, bhi = score_ci(bw, bl, bd)
    # significant = candidate CI lower bound strictly above champion point score
    significant = clo > bs and (cw + cl + cd) >= min_games
    return significant, {'cand_score': cs, 'cand_ci': [clo, chi],
                         'base_score': bs, 'base_ci': [blo, bhi],
                         'cand_elo': elo(cs), 'base_elo': elo(bs),
                         'delta_elo': elo(cs) - elo(bs)}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cycles', type=int, default=3)
    ap.add_argument('--games', type=int, default=16)
    ap.add_argument('--sf-elo', type=int, default=1400)
    ap.add_argument('--tc', type=float, default=0.2)
    ap.add_argument('--analyse-depth', type=int, default=10)
    a = ap.parse_args()

    st = load_state()
    print(f"champion={st['champion']} starting at cycle {st['cycle']}")

    # measure champion baseline once per run
    ctag = f"champ_c{st['cycle']}"
    print(f"\n[baseline] {st['champion']} vs SF18@{a.sf_elo}, {a.games} games")
    bw, bl, bd, out = play(ctag, f"{ENGDIR}/bin/{st['champion']}", a.games, a.sf_elo, a.tc)
    print(out.strip().splitlines()[-1] if out.strip() else 'no output')
    bs, blo, bhi = score_ci(bw, bl, bd)
    print(f"baseline score={bs:.3f} Elo={elo(bs):+.0f}")

    print("\n[analysis] mining weaknesses from champion games")
    print(analyse(ctag, a.analyse_depth, a.games))

    for c in range(a.cycles):
        st['cycle'] += 1
        cyc = st['cycle']
        print(f"\n{'='*60}\nCYCLE {cyc}\n{'='*60}")
        # NOTE: candidate generation is driven by the analysis report.
        # Each candidate must be introduced as a real source change, then built here.
        cand_bin = f'chess_engine_c{cyc}'
        ok, err = build(cand_bin)
        if not ok:
            print(f"build failed:\n{err}"); continue
        tag = f'cand_c{cyc}'
        cw, cl, cd, out = play(tag, f'{ENGDIR}/bin/{cand_bin}', a.games, a.sf_elo, a.tc)
        print(out.strip().splitlines()[-1] if out.strip() else 'no output')
        sig, stats = gate((cw, cl, cd), (bw, bl, bd), a.games)
        print(json.dumps(stats, indent=1))
        rec = {'cycle': cyc, 'candidate': cand_bin,
               'cand_wld': [cw, cl, cd], 'base_wld': [bw, bl, bd],
               'significant': sig, **stats,
               'ts': datetime.datetime.now().isoformat()}
        if sig:
            print(f"ACCEPTED: {cand_bin} is a significant improvement -> new champion")
            st['champion'] = cand_bin
            bw, bl, bd = cw, cl, cd
        else:
            print(f"REJECTED: no statistically significant gain; keeping {st['champion']}")
            st['rejected'].append(cand_bin)
        st['history'].append(rec)
        save_state(st)

    print(f"\nfinal champion: {st['champion']}")
    json.dump(st, open(f'{LAB}/reports/loop_state.json', 'w'), indent=1)

if __name__ == '__main__':
    main()
