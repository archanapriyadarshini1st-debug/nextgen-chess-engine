#!/usr/bin/env python3
"""Explain WHY a move is best, WHEN to play it, and WHY the engine's choice differed.

Produces human-readable coaching text plus a machine-readable JSONL record that
can be fed back into training (why_best / when_to_play labels).
"""
import chess, chess.engine, chess.pgn, json, argparse, os

SF = os.environ.get('SF_BIN', '/home/user/sf/stockfish18')
NG = os.environ.get('NG_ENGINE', '/home/user/webapp/engine/bin/chess_engine')

PIECE_NAME = {chess.PAWN:'pawn', chess.KNIGHT:'knight', chess.BISHOP:'bishop',
              chess.ROOK:'rook', chess.QUEEN:'queen', chess.KING:'king'}

CENTER = {chess.D4, chess.E4, chess.D5, chess.E5}
BIG_CENTER = CENTER | {chess.C3,chess.D3,chess.E3,chess.F3,chess.C4,chess.F4,
                       chess.C5,chess.F5,chess.C6,chess.D6,chess.E6,chess.F6}

def phase_of(b):
    n = len(b.piece_map())
    if b.fullmove_number <= 12: return 'opening'
    return 'middlegame' if n > 12 else 'endgame'

def material(b, color):
    v = {chess.PAWN:100, chess.KNIGHT:320, chess.BISHOP:330, chess.ROOK:500, chess.QUEEN:900}
    return sum(v.get(p.piece_type,0) for p in b.piece_map().values() if p.color==color)

def describe_move(b, mv):
    """Structured, rule-based reasons a move is good - independent of Stockfish internals."""
    reasons = []
    pc = b.piece_at(mv.from_square)
    if pc is None: return reasons
    name = PIECE_NAME[pc.piece_type]

    if b.is_capture(mv):
        victim = b.piece_at(mv.to_square)
        if b.is_en_passant(mv):
            reasons.append("captures a pawn en passant")
        elif victim:
            reasons.append(f"captures the {PIECE_NAME[victim.piece_type]} on {chess.square_name(mv.to_square)}")
    if b.gives_check(mv):
        reasons.append("gives check, forcing the opponent's reply")
    if b.is_castling(mv):
        reasons.append("castles: king to safety and rook activated")
    if mv.promotion:
        reasons.append(f"promotes to a {PIECE_NAME[mv.promotion]}")

    # central control / development
    if mv.to_square in CENTER and pc.piece_type == chess.PAWN:
        reasons.append("occupies the centre with a pawn, claiming space")
    if pc.piece_type in (chess.KNIGHT, chess.BISHOP):
        back = 0 if pc.color == chess.WHITE else 7
        if chess.square_rank(mv.from_square) == back:
            reasons.append(f"develops the {name} off the back rank")
        if mv.to_square in BIG_CENTER:
            reasons.append(f"places the {name} on an active central square")

    # safety check: does it hang the piece?
    b.push(mv)
    hanging = b.is_attacked_by(not pc.color, mv.to_square) and not b.is_attacked_by(pc.color, mv.to_square)
    in_check_after = b.is_check()
    b.pop()
    if hanging:
        reasons.append(f"NOTE: the {name} lands on an undefended attacked square")

    # threats created
    b.push(mv)
    threats = []
    for sq, piece in b.piece_map().items():
        if piece.color == (not pc.color) and piece.piece_type != chess.PAWN:
            if b.is_attacked_by(pc.color, sq) and not b.is_attacked_by(not pc.color, sq):
                threats.append(PIECE_NAME[piece.piece_type])
    b.pop()
    if threats:
        reasons.append(f"creates a threat against the {', '.join(sorted(set(threats)))}")

    if not reasons:
        reasons.append("a quiet improving move: better piece placement without immediate contact")
    return reasons

def when_to_play(b, mv, ph):
    """Describe the situational trigger for the move."""
    triggers = []
    if ph == 'opening':
        triggers.append("in the opening, while development and the centre are still being contested")
    elif ph == 'middlegame':
        triggers.append("in the middlegame, when pieces are active and concrete threats decide")
    else:
        triggers.append("in the endgame, where king activity and pawn promotion dominate")
    if b.is_check():
        triggers.append("specifically when you are in check and must resolve it")
    if b.is_capture(mv):
        triggers.append("when material can be won or a favourable trade is available")
    md = material(b, chess.WHITE) - material(b, chess.BLACK)
    md = md if b.turn == chess.WHITE else -md
    if md < -200:
        triggers.append("when you are behind on material and need activity/counterplay rather than passivity")
    elif md > 200:
        triggers.append("when you are ahead on material and simplification favours you")
    return triggers

def analyse_position(sf, ng, board, depth_sf=14, depth_ng=8):
    info = sf.analyse(board, chess.engine.Limit(depth=depth_sf), multipv=3)
    best = info[0]['pv'][0]
    best_cp = info[0]['score'].pov(board.turn).score(mate_score=10000)
    alts = []
    for i in info[1:]:
        if i.get('pv'):
            alts.append((i['pv'][0], i['score'].pov(board.turn).score(mate_score=10000)))
    ours = None
    if ng is not None:
        try:
            ours = ng.play(board, chess.engine.Limit(depth=depth_ng)).move
        except Exception:
            ours = None
    return best, best_cp, alts, ours

def explain(board, best, best_cp, alts, ours, sf, depth_sf):
    ph = phase_of(board)
    out = {
        'fen': board.fen(), 'phase': ph,
        'best_move': board.san(best), 'best_uci': best.uci(), 'best_eval_cp': best_cp,
        'why_best': describe_move(board, best),
        'when_to_play': when_to_play(board, best, ph),
        'alternatives': [{'move': board.san(m), 'eval_cp': c} for m, c in alts],
    }
    if ours is not None and ours in board.legal_moves:
        out['engine_move'] = board.san(ours)
        board.push(ours)
        after = sf.analyse(board, chess.engine.Limit(depth=depth_sf))
        ours_cp = -after['score'].pov(board.turn).score(mate_score=10000)
        board.pop()
        out['engine_eval_cp'] = ours_cp
        out['cp_loss'] = best_cp - ours_cp
        out['matches_best'] = (ours == best)
        if not out['matches_best']:
            out['why_engine_move_worse'] = describe_move(board, ours)
    return out

def render(e):
    L = []
    L.append(f"Position ({e['phase']}): {e['fen']}")
    L.append(f"  BEST: {e['best_move']}  (eval {e['best_eval_cp']:+d}cp)")
    L.append("  WHY it is best:")
    for r in e['why_best']: L.append(f"    - {r}")
    L.append("  WHEN to play this kind of move:")
    for r in e['when_to_play']: L.append(f"    - {r}")
    if e.get('alternatives'):
        alts = ", ".join(f"{a['move']} ({a['eval_cp']:+d})" for a in e['alternatives'])
        L.append(f"  Alternatives: {alts}")
    if 'engine_move' in e:
        tag = "MATCHES best" if e['matches_best'] else f"loses {e['cp_loss']}cp vs best"
        L.append(f"  NextGen played: {e['engine_move']} ({tag})")
        if not e['matches_best']:
            L.append("  Why NextGen's move is worse:")
            for r in e.get('why_engine_move_worse', []): L.append(f"    - {r}")
    return "\n".join(L)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pgn'); ap.add_argument('--fen')
    ap.add_argument('--max-positions', type=int, default=30)
    ap.add_argument('--depth', type=int, default=14)
    ap.add_argument('--out', default='/home/user/webapp/lab/reports/explanations.jsonl')
    ap.add_argument('--text-out', default='/home/user/webapp/lab/reports/explanations.txt')
    ap.add_argument('--only-mistakes', action='store_true')
    a = ap.parse_args()

    sf = chess.engine.SimpleEngine.popen_uci(SF); sf.configure({'Hash':128})
    ng = None
    try: ng = chess.engine.SimpleEngine.popen_uci(NG)
    except Exception: pass

    boards = []
    if a.fen:
        boards.append(chess.Board(a.fen))
    elif a.pgn:
        with open(a.pgn) as f:
            while len(boards) < a.max_positions:
                g = chess.pgn.read_game(f)
                if g is None: break
                b = g.board()
                ngw = g.headers.get('White') == 'NextGen'
                for i, node in enumerate(g.mainline()):
                    if (b.turn == chess.WHITE) == ngw and not b.is_game_over():
                        boards.append(b.copy())
                        if len(boards) >= a.max_positions: break
                    b.push(node.move)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    n = 0
    with open(a.out, 'w') as jf, open(a.text_out, 'w') as tf:
        for b in boards:
            try:
                best, cp, alts, ours = analyse_position(sf, ng, b, a.depth)
                e = explain(b, best, cp, alts, ours, sf, a.depth)
            except Exception:
                continue
            if a.only_mistakes and e.get('matches_best', False):
                continue
            jf.write(json.dumps(e) + "\n")
            txt = render(e)
            tf.write(txt + "\n\n")
            if n < 6: print(txt + "\n")
            n += 1
    sf.quit()
    if ng:
        try: ng.quit()
        except Exception: pass
    print(f"wrote {n} explanations -> {a.out}")

if __name__ == '__main__':
    main()
