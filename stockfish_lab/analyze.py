#!/usr/bin/env python3
"""Analyze games: find blunders, phase weaknesses, eval honesty, SF tendencies."""
import chess, chess.pgn, chess.engine, json, os, argparse, collections, statistics

SF='/home/user/sf/stockfish18'; LAB='/home/user/webapp/lab'

def phase_of(b):
    n=len(b.piece_map())
    if b.fullmove_number<=12: return 'opening'
    return 'middlegame' if n>12 else 'endgame'

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--tag',required=True); ap.add_argument('--depth',type=int,default=12)
    ap.add_argument('--max-games',type=int,default=16)
    a=ap.parse_args()
    sf=chess.engine.SimpleEngine.popen_uci(SF); sf.configure({'Hash':128,'Threads':2})
    lim=chess.engine.Limit(depth=a.depth)
    games=[]
    with open(f'{LAB}/pgn/{a.tag}.pgn') as f:
        while len(games)<a.max_games:
            g=chess.pgn.read_game(f)
            if g is None: break
            games.append(g)
    blunders=[]; phase_loss=collections.defaultdict(list); evalgap=[]
    piece_blunder=collections.Counter(); sf_style=collections.Counter()
    for gi,g in enumerate(games):
        ng_white = g.headers['White']=='NextGen'
        b=g.board()
        for node in g.mainline():
            mv=node.move; ours=(b.turn==chess.WHITE)==ng_white
            ph=phase_of(b)
            if ours:
                info=sf.analyse(b,lim,multipv=2)
                best=info[0]['pv'][0]
                sb=info[0]['score'].pov(b.turn).score(mate_score=10000)
                # eval after our actual move
                b.push(mv)
                ai=sf.analyse(b,lim)
                sa=-ai['score'].pov(b.turn).score(mate_score=10000)
                b.pop()
                loss=sb-sa
                phase_loss[ph].append(loss)
                if loss>=150:
                    blunders.append({'game':gi,'ply':b.ply(),'fen':b.fen(),'played':b.san(mv),
                        'best':b.san(best),'loss_cp':loss,'phase':ph,
                        'sf_eval_before':sb,'sf_eval_after':sa,
                        'was_capture':b.is_capture(mv),'best_was_capture':b.is_capture(best),
                        'gave_check':b.gives_check(mv),'best_gives_check':b.gives_check(best)})
                    pc=b.piece_at(mv.from_square)
                    if pc: piece_blunder[pc.symbol().upper()]+=1
            else:
                # characterize SF's move style
                pc=b.piece_at(mv.from_square)
                tags=[]
                if b.is_capture(mv): tags.append('capture')
                if b.gives_check(mv): tags.append('check')
                if pc and pc.piece_type==chess.PAWN: tags.append('pawn_move')
                if b.is_castling(mv): tags.append('castle')
                if not tags: tags.append('quiet')
                for t in tags: sf_style[f'{ph}:{t}']+=1
            b.push(mv)
    sf.quit()
    out={'tag':a.tag,'games':len(games),'blunders':blunders,
         'avg_cp_loss_by_phase':{k:round(statistics.mean(v),1) for k,v in phase_loss.items() if v},
         'moves_by_phase':{k:len(v) for k,v in phase_loss.items()},
         'blunder_count_by_phase':dict(collections.Counter(x['phase'] for x in blunders)),
         'blunders_by_piece':dict(piece_blunder),
         'sf_move_style':dict(sf_style)}
    json.dump(out,open(f'{LAB}/reports/{a.tag}_analysis.json','w'),indent=1)
    print(f"games={len(games)} blunders(>=150cp)={len(blunders)}")
    print('avg cp loss/phase:',out['avg_cp_loss_by_phase'])
    print('moves/phase:',out['moves_by_phase'])
    print('blunders/phase:',out['blunder_count_by_phase'])
    print('blunders/piece:',out['blunders_by_piece'])
    print('\nTop 12 blunders:')
    for x in sorted(blunders,key=lambda y:-y['loss_cp'])[:12]:
        print(f"  g{x['game']} {x['phase']:11s} played {x['played']:7s} best {x['best']:7s} loss {x['loss_cp']:6d}cp")
if __name__=='__main__': main()
