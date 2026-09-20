# NextGen vs Stockfish 19 — Elo, losses, and saved best moves

**Date:** 2026-09-20  
**Hardware:** 2 cores, 1.9 GB RAM  
**Opponent:** official **Stockfish 19** (`id name Stockfish 19`, SFNNv16, UCI_Elo 1320–3190, full strength ~3600 CCRL)

This pass measured real Elo, played Stockfish 19 at **full strength**, and after every loss asked SF19 what NextGen should have played. Those moves are stored in an experience book the engine now loads automatically.

---

## Current Elo: about **1200** (not 3000)

Previous repo docs (`README.md`, `FINAL_100K_REPORT.md`) guessed 2500–3100 Elo from feature checklists. That was not measured. A live ladder against Stockfish 19 with `UCI_LimitStrength` on, **120 ms/move each side**, 8 games per level, mixed openings, both colours:

| SF19 UCI_Elo | NextGen W–L–D | Score | Implied Elo |
|-------------:|---------------|------:|------------:|
| 1320 | 2–5–1 | 31.3% | **1200** |
| 1400 | 1–7–0 | 12.5% | **1120** |
| 1600 | 0–6–2 | 12.5% | ~1260 |
| 1800 | 0–7–1 | 6.3% | (too few points) |
| 2000 | 0–8–0 | 0% | clearly weaker than 2000 |

**Combined estimate: ~1250. Best-informed estimate (vs 1320–1400): ~1180–1200.**

NextGen **can beat** a 1320-capped SF19 (it did, twice, including a 51-ply checkmate as White). It is **crushed** at 2000+. It is a club-player engine, not a Stockfish-class one.

Raw JSON: `datasets/elo_measurement.json`

---

## Stockfish 19 MAX (uncapped)

120 games, NextGen 100 ms / SF19 40 ms, 20 openings, colours flipped.

| | |
|--|--|
| Result | **0 wins, 119 losses, 1 draw** (threefold) |
| Score | 0.4% |
| Typical finish | checkmate around ply 30–70 |
| Throughput | **0.30 games/s** including post-loss analysis |

A 0–119–1 score vs ~3600 Elo does **not** mean NextGen is 2800. It only means it is nowhere near Stockfish. The ladder above is the rating.

One draw (game 119) is real: threefold repetition, not a crash.

---

## What is saved after a loss (like a normal engine)

After every loss, SF19 analysed every NextGen move (depth 8) and compared it to SF19’s best move.

| File | What it is |
|------|------------|
| `datasets/missed_best_moves.jsonl` | **1940** records: FEN, move we played, SF19 best, cp loss, phase, piece, why the best move is better |
| `datasets/experience_book.txt` | **1010** unique positions, engine-readable. Auto-loaded on `isready`. Next time that position appears, NextGen plays the saved best move. |
| `datasets/games_vs_sf19.pgn` | All 120 games, PGN |
| `datasets/games_vs_sf19.jsonl` | Per-game W/L/D + blunder counts |
| `datasets/weaknesses.json` | Aggregated weakness report |

Example records (from real losses):

```text
startpos  we played b1c3   SF19 wanted e2e4   (~30cp, opening)
.../3q4/8/5N2/... b  we played d5f3   SF19 wanted c8g4   (−792cp, hanging queen)
... we played g4f2    SF19 wanted g4f6   (knight retreat, opening)
... we pushed a wing pawn; SF19 wanted to castle
```

Experience-book line format (loaded by the engine):

```text
<fen> <best_uci> <weight>
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 e2e4 400
```

Verified: `setoption name OwnBook value true` → engine answers `bestmove d2d4` / `e2e4` from the book instead of searching.

---

## Weaknesses SF19 exposed

From 1940 labelled moves (904 with 50–4999 cp loss; 118 were “walked into mate”, cp_loss ≥ 5000):

### By phase (real mistakes, 50–4999 cp)

| Phase | Moves | Avg cp lost | Blunders (≥300 cp) |
|-------|------:|------------:|-------------------:|
| Opening | 377 | **240** | **113** |
| Middlegame | 518 | 159 | 59 |
| Endgame | 9 | 167 | 1 |

Games almost never reach a quiet endgame — SF19 mates in the middlegame. The 118 mate-walks sit mostly there: once the position is lost, search fails to find the most resilient defence.

### By piece

Pawns (228) and knights (208) are the most common mistake pieces. Queen mistakes are the most expensive (avg ~275 cp).

### Recurring patterns

1. **Does not occupy the centre.** From startpos it still prefers `b1c3` over `e2e4` (~30 cp, every time). Book now overrides this.
2. **Wing-pawn pushes** (`a4`/`h4`/`g4`) instead of developing or castling — 109 labelled cases. This was the old pawn PST (file-agnostic, so `a2a4` ≈ `e2e4`). Fixed in eval this session; book also punishes leftovers.
3. **Premature knight hops** (`f3g5`, `g5f7`, `g4f2`) instead of developing the bishop or retreating.
4. **Queen raids in the opening** that hang the queen (`d5f3` instead of `c8g4`).
5. **Does not castle** when SF19 would (28 times).
6. **Missed captures and checks** in the middlegame — the 21% of SF19 middlegame moves that are checks is exactly where NextGen bleeds.

---

## Engine fixes applied this session (needed before the match)

The tree as cloned had real bugs. Matches would have been noise without these:

1. **`bestmove 0000` forfeit.** Root search initialised `local_best = -INF` (−30000). Mate scores are ~−31999, so `score > -INF` never fired when every move was getting mated. NextGen emitted a null move and lost on illegal move. Fixed: seed a legal fallback, compare with `have_move || score > best`. Verified on the two FINDINGS.md positions → `e1e2`, `b1c1`.
2. **Stopped wrapping Stockfish as the evaluator.** `evaluation/eval.cpp` used to `fork()` a Stockfish process inside `evaluate()`. That is not “our” engine and is far too slow. Disabled.
3. **Real piece-square tables.** Pawns now want e4/d4, knights want the centre, king wants to castle. First move in a raw search is `g1f3` / `b1c3`, not `a2a4`.
4. **UCI stdout unitbuf.** Without it, python-chess hangs on a pipe.
5. **Experience book loader.** Text book now parses a full 6-field FEN + move + weight. UCI option `ExperienceFile`. Auto-load `datasets/experience_book.txt` on `isready`.

Perft still passes: startpos 20/400/8902/197281, Kiwipete 48/2039/97862.

---

## Why this is not a million games (and how to get there)

This machine plays **~0.30 analysed games/s** vs SF19 max (2 cores).

| Volume | Wall time here |
|--------|----------------|
| 120 (this session) | ~7 minutes |
| 1,000 | ~1 hour |
| 10,000 | ~9 hours |
| 100,000 | ~4 days |
| **1,000,000** | **~39 days, 24/7** |

A million *analysed* games against uncapped Stockfish 19 is not a one-session job. The pipeline is written to run unattended and resume (append-only JSONL + experience book):

```bash
# Elo ladder (limited SF19)
python3 tools/sf19_max_train.py elo --games-per-level 20 --our-ms 120 --sf-ms 120

# Keep learning from SF19 MAX (Ctrl+C safe; files append)
python3 tools/sf19_max_train.py train --games 1000000 --our-ms 100 --sf-ms 40 --analyse-depth 8
```

Stockfish itself was trained on **hundreds of billions of positions** via Fishtest, not on a laptop playing sequential games. The useful unit here is **labelled positions after losses**, which we already write. 1940 high-quality SF19 labels beat 100k self-play games at depth 4 (the old loop that overfit to loss 0.0).

---

## How to run it

```bash
# Stockfish 19 (already at bin/stockfish19 in this workspace)
# Engine
g++ -std=c++20 -O3 -march=native -pthread -I. \
  core/position.cpp core/movegen.cpp core/zobrist.cpp \
  search/search.cpp search/tt.cpp search/heuristics.cpp \
  evaluation/eval.cpp nnue/nnue.cpp \
  uci/uci.cpp logging/game_log.cpp tb/syzygy.cpp opening/book.cpp \
  main.cpp -o bin/chess_engine

# Use the learned book
printf 'uci
setoption name ExperienceFile value datasets/experience_book.txt
setoption name OwnBook value true
isready
position startpos
go depth 10
quit
' | ./bin/chess_engine
```

---

## Bottom line

| Claim in old docs | Measured now |
|-------------------|--------------|
| 2500–3100 Elo, “only training left” | **~1200 Elo** vs SF19 skill levels |
| Ready to take on Stockfish 19 | **0/119/1** vs SF19 max |
| Self-play loss 0.0 = strong net | Overfit; not used as the eval this session |

What *is* in place, and working:

- Honest Elo ladder against Stockfish 19
- Loss → SF19 oracle → **saved best move** → experience book the engine actually plays
- Weakness map: centre control, wing pawns, early queen raids, knight lunges, not castling, mate-walks in the middlegame
- A trainer you can leave running toward 1M games: `tools/sf19_max_train.py`
