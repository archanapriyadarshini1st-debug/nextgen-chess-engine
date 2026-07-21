#!/usr/bin/env python3
"""
SPRT testing harness - bayesian Elo estimation
"""
import math, argparse

def elo_to_prob(elo):
    return 1/(1+10**(-elo/400))

def sprt_llr(wins, losses, draws, elo0, elo1):
    # simplified LLR
    p0 = elo_to_prob(elo0)
    p1 = elo_to_prob(elo1)
    total = wins+losses+draws
    if total==0: return 0
    # win prob for each
    score = wins + draws*0.5
    p = score/total
    # avoid 0
    p = max(0.001,min(0.999,p))
    llr = wins*math.log(p1/p0) + losses*math.log((1-p1)/(1-p0)) + draws*math.log( (0.5) )
    return llr

def estimate_elo(wins, losses, draws):
    total=wins+losses+draws
    if total==0: return 0
    score = (wins+draws*0.5)/total
    if score<=0 or score>=1: return 0
    return -400*math.log10(1/score-1)

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("--wins",type=int,default=0)
    ap.add_argument("--losses",type=int,default=0)
    ap.add_argument("--draws",type=int,default=0)
    ap.add_argument("--elo0",type=float,default=0)
    ap.add_argument("--elo1",type=float,default=5)
    args=ap.parse_args()
    print(f"Elo est: {estimate_elo(args.wins,args.losses,args.draws):.2f}")
    print(f"LLR: {sprt_llr(args.wins,args.losses,args.draws,args.elo0,args.elo1):.2f}")
