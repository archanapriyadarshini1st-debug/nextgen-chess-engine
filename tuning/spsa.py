#!/usr/bin/env python3
import argparse, json, math, os, random, subprocess, tempfile
from pathlib import Path
DEFAULT_PARAMS={"lmr_base":0.75,"lmr_mult":0.35,"futility_base":80,"futility_mult":150,"reverse_futility_base":60,"reverse_futility_mult":120,"null_R_base":3,"history_bonus_base":16}

def perturb(p,c): return {k:v+(1 if random.getrandbits(1) else -1)*c*max(abs(v),1.0) for k,v in p.items()}
def score_games(text):
    w=l=d=0
    for line in text.splitlines():
        low=line.lower()
        if 'white wins' in low: w+=1
        elif 'black wins' in low: l+=1
        elif 'draw' in low: d+=1
    return w,l,d

def run_match(engine,baseline,games,tc,book=None):
    cmd=['cutechess-cli','-engine',f'name= candidate cmd={engine}','-engine',f'name= baseline cmd={baseline}','-each',f'tc={tc}','-games',str(games),'-rounds',str(max(1,games//2)),'-repeat','-recover','-concurrency','1','-pgnout',os.devnull]
    if book: cmd += ['-openings',f'file={book}', 'format=pgn', 'order=random']
    try:
        r=subprocess.run(cmd,text=True,capture_output=True,timeout=max(300,games*30),check=False)
        return score_games(r.stdout+'\n'+r.stderr)
    except (OSError,subprocess.TimeoutExpired): return 0,0,0

def sprt(w,l,d,elo0=0,elo1=5,alpha=0.05,beta=0.05):
    n=w+l+d
    if not n:return 0.0
    p=max(1e-6,min(1-1e-6,(w+0.5*d)/n)); elo=-400*math.log10(1/p-1)
    llr=(elo-elo0)/(elo1-elo0)*math.log((1-alpha)/beta)
    return llr

def evaluate_params(engine,baseline,params,games,tc,book=None):
    return run_match(engine,baseline,games,tc,book)
def spsa_loop(engine,baseline,iterations,games,tc,book=None):
    p=DEFAULT_PARAMS.copy(); a=0.05; c=0.1
    for it in range(iterations):
        plus=perturb(p,c); minus=perturb(p,-c)
        wp,lp,dp=evaluate_params(engine,baseline,plus,games,tc,book); wm,lm,dm=evaluate_params(engine,baseline,minus,games,tc,book)
        ep=sprt(wp,lp,dp); em=sprt(wm,lm,dm)
        g=(ep-em)/(2*c)
        for k in p:
            p[k]+=a*g
            if 'base' in k:p[k]=max(0.1,min(10000,p[k]))
            elif 'mult' in k:p[k]=max(0.01,min(10000,p[k]))
        Path('tuning/params.json').write_text(json.dumps(p,indent=2))
        print(f'iter={it} plus={wp}-{lp}-{dp} minus={wm}-{lm}-{dm} params={p}')
        a*=0.97;c*=0.99
    return p
if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('--engine',required=True); ap.add_argument('--baseline',required=True); ap.add_argument('--iter',type=int,default=10); ap.add_argument('--games',type=int,default=200); ap.add_argument('--tc',default='10+0.1'); ap.add_argument('--book'); a=ap.parse_args(); spsa_loop(a.engine,a.baseline,a.iter,a.games,a.tc,a.book)
