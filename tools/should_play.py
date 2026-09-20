#!/usr/bin/env python3
"""Compare NextGen's move vs Stockfish 19's move on a set of positions."""
import json
from pathlib import Path
import chess, chess.engine

ROOT = Path(__file__).resolve().parent.parent
OUR = ROOT / "bin" / "chess_engine"
def _find_sf():
    for p in (ROOT / "bin" / "stockfish19", Path("/tmp/sf19/stockfish19"), Path("/tmp/stockfish19")):
        if p.exists():
            return p
    return ROOT / "bin" / "stockfish19"
SF = _find_sf()

POSITIONS = [
    ("startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
    ("after e4", "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"),
    ("Italian", "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4"),
    ("Sicilian", "rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3"),
    ("KID", "rnbq1rk1/ppppppbp/5np1/8/2PPP3/2N5/PP3PPP/R1BQKBNR w KQ - 0 5"),
    ("hanging queen (from losses)", "rnb1kbnr/ppp1pppp/8/3q4/8/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 3"),
    ("should castle", "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 6"),
    ("endgame KPk", "8/8/8/4k3/8/4K3/4P3/8 w - - 0 1"),
    ("mate in 1", "6k1/5ppp/8/8/8/8/5PPP/6RK w - - 0 1"),
    ("middlegame tactics", "r1bqr1k1/pp3pbp/2np1np1/2p1p3/2P1P3/2NP1NP1/PP3PBP/R1BQ1RK1 w - - 0 9"),
]

def main():
    our = chess.engine.SimpleEngine.popen_uci(str(OUR))
    sf = chess.engine.SimpleEngine.popen_uci(str(SF))
    our.configure({"Hash": 32, "Threads": 1, "OwnBook": False})
    sf.configure({"Hash": 32, "Threads": 1})
    print(f"{'pos':28s} {'NextGen':8s} {'SF19':8s} {'match':5s}  SF eval   note")
    match = 0
    rows = []
    for name, fen in POSITIONS:
        b = chess.Board(fen)
        ng = our.play(b, chess.engine.Limit(depth=8))
        info = sf.analyse(b, chess.engine.Limit(depth=12))
        best = info["pv"][0] if info.get("pv") else None
        cp = info["score"].white().score(mate_score=10000)
        ok = ng.move == best
        if ok:
            match += 1
        note = ""
        if best and ng.move != best:
            note = f"should play {best.uci()}"
        print(f"{name:28s} {str(ng.move):8s} {str(best):8s} {'YES' if ok else 'no':5s}  {cp:7}  {note}")
        rows.append({"name": name, "fen": fen, "nextgen": ng.move.uci() if ng.move else None,
                     "sf19": best.uci() if best else None, "match": ok, "sf_cp_white": cp})
    print(f"\nagreement {match}/{len(POSITIONS)}")
    Path("/home/user/nextgen-chess-engine/datasets/should_play.json").write_text(json.dumps(rows, indent=2))
    our.quit(); sf.quit()

if __name__ == "__main__":
    main()
