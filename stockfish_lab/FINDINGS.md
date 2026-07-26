# NextGen vs Stockfish 18 — Improvement Loop, Cycle 0 Findings

Date: 2026-07-26
Opponent: **Stockfish 18** (official release binary, `id name Stockfish 18`, 112MB, avx2)
Engine: `nextgen-chess-engine` @ `main` (fetched via Composio GitHub connection)

---

## 0. Environment reality check

| item | value |
|---|---|
| sandbox cores | 2 |
| NextGen speed | **~68k nodes/sec**, depth 8 in 2s |
| Stockfish 18 speed | millions of nps, depth 20+ in the same 2s |

This ~50-100x search-speed gap is the hard ceiling on this hardware and frames
every result below. Beating full-strength SF18 is not achievable here; the loop
is therefore calibrated against **rate-limited Stockfish** (`UCI_LimitStrength`)
so that improvements are measurable rather than drowned out.

---

## 1. The repo did not build — 3 blocking defects fixed

`core/bitboard.cpp` never compiled as committed:

1. `U64` and `Square` used but never typedef'd → added.
2. `make_sq()` / `square_bb()` called but never defined; `file_of`/`rank_of`
   live in `namespace chess` and were unqualified → added + `using`.
3. `ROOK_MAGICS_NUM` / `BISHOP_MAGICS_NUM` declared `[64]` but initialised with
   **66 entries** → truncated to 64.
4. `pawn_attacks[WHITE]` / `[BLACK]` — those constants don't exist in
   `types.h` (only castling-rights constants) → indexed `[0]`/`[1]`.

The claim in `README.md` / `CHECKLIST_STOCKFISH19.md` that the engine is at
"3000-3100 Elo, only training left" could not have been verified against this
tree, because this tree does not compile.

---

## 2. THE critical bug: `bestmove 0000` → instant forfeit

**Symptom.** In the first 8-game probe, **5 of 8 games** ended
`illegal:ng:0000` — the engine emitted a null move and forfeited.

**Reproduction.** Every failing position had the engine **in check with 1-2
legal moves**:

```
3r1r2/6Np/2p3k1/P7/8/4P1PP/8/2q1K2R w - - 0 33      (in check, 1 legal move)
8/2pk2N1/p7/4p3/P3b1PP/1P6/1r3r2/1K4R1 w - - 2 40   (in check, 2 legal moves)
r4b1r/2Q1kpp1/1p2bn2/pB2p1Bp/P6P/2N5/1PP2PP1/2KR4 b - - 2 19
2b1k3/1pp4p/8/P4pq1/4p3/5n1P/P2rKP2/R1Q2B2 w - - 7 23
r3kbr1/4p3/2Q5/p2p1Rpp/6n1/2N1P3/PP4PP/4R1K1 b - - 2 23
```

**Root cause** (`search/search.cpp`, `think()`): the root loop initialises
`local_best = -INF` and only records a move via `if (score > local_best)`.
When the side to move is getting mated, *every* root move returns exactly
`-INF`, the comparison is never true, `local_move` stays default-constructed,
and UCI prints `bestmove 0000`.

**Fix** (committed):
- compute a guaranteed-legal `fallback` move immediately after root generation
  and seed `r.best_move` with it;
- replace `score > local_best` with `!have_move || score > local_best` in
  **both** the sequential and the `std::async` parallel root loops;
- never overwrite `r.best_move` with a null move.

**Verified**: all 5 positions now return legal moves (`e1e2`, `b1c1`, `e6d7`,
`c1d2`, `e8d8`); a 16-game rerun produced **zero** abnormal terminations.

**Measured effect** vs SF18 @ Elo 1400, 0.2s/move, 16 games:

| build | W | L | D | score |
|---|---|---|---|---|
| before fix (8 games) | 0 | 7 | 1 | 0.062 |
| after fix | 3 | 12 | 1 | **0.219** |

This single fix is worth roughly **+250 Elo** at this level — it was silently
throwing away about a third of all games.

---

## 3. Where the remaining strength is lost

97 blunders ≥150cp across 16 games (SF18 depth-10 reference):

| phase | moves | avg cp lost | blunders |
|---|---|---|---|
| opening | 192 | 77 | 29 |
| **middlegame** | 335 | **566** | **65** |
| endgame | 55 | 164 | 3 |

Blunders by piece: **P 35**, N 18, Q 12, K 12, B 12, R 8.

Loss distribution: 77 in the 150-1000cp band, **20 outright walks into forced
mate** (>5000cp). The mate-walks all occur in positions already lost
(-338 to -631) — a symptom of the search failing to find the most resilient
defence rather than of fresh mistakes.

### Eval calibration (60 positions, NextGen d7 vs SF18 d12)
- mean |difference| = **521cp**, median **232cp**
- **sign disagreements = 0/60 (0%)**

So the evaluation gets the *direction* right but the *magnitude* badly wrong —
it systematically **under-estimates how bad a bad position is** (e.g. reports
+141cp where SF18 says -338cp). Compressed evals blunt the search's incentive
to fight.

### Opening behaviour
The engine's **most-played first-10 move was `a4` (7×)**, and **20% of its
opening moves were rook/wing pawn pushes** (a4/h4/a5/h5/b3/b6).

Cause found in `evaluation/eval.cpp`:
```c
case Piece::WP: return r * 9 + center/2;   // file-agnostic!
```
`a2a4` scores **31** vs `e2e4` **33** — essentially indistinguishable, so the
engine has no reason to prefer the centre.

---

## 4. Stockfish 18 behavioural profile (learned only from its moves)

Move-type distribution observed over these games:

| phase | profile |
|---|---|
| opening | pawn_move 39%, quiet 36%, capture 18%, check 5%, castle 2% |
| middlegame | quiet 46%, capture 23%, **check 21%**, pawn_move 10% |
| endgame | **quiet 91%**, check 4%, capture 4%, pawn_move 2% |
| overall | quiet 46.3%, capture 19.7%, pawn 18.4%, check 14.5% |

Readable tendencies:
- **Middlegame is where it hurts you** — 21% of its middlegame moves are checks;
  it converts advantages through forcing sequences. This coincides exactly with
  our own worst phase (566cp avg loss).
- **Endgames are 91% quiet manoeuvring** — it wins endings by slow improvement,
  not tactics. An engine with a shallow-but-sound endgame eval concedes least here
  (our endgame avg loss is only 164cp over 55 moves), so **steering toward
  simplified positions is the most exploitable direction**.
- Opening play is pawn-structure led, not gambit-led.

---

## 5. Candidate tested and REJECTED by the statistical gate

**Candidate v2**: replaced the broken pawn PST with proper piece-square tables
(centre-weighted pawns, edge-penalised knights, king safety table).

Behaviour did change as intended (first move `b1c3` → `g1f3`), but:

| build | W | L | D | score | 95% CI |
|---|---|---|---|---|---|
| v1 (null-fix) | 3 | 12 | 1 | 0.219 | [0.026, 0.412] |
| v2 (+PSQT) | 0 | 14 | 2 | 0.062 | [-0.019, 0.144] |

The confidence intervals overlap, so the difference is **not statistically
significant** at 16 games — and the point estimate is worse. Per rule 7 the
change was **rejected and not merged**; `evaluation/eval.cpp.v1` retains the
original for reference and the PSQT work is preserved on disk for retesting at
larger sample sizes.

This is the loop working as designed: a plausible-sounding improvement was
measured, failed to prove itself, and was thrown away instead of being assumed.

---

## 6. Priority list (evidence-ordered)

1. **Search speed.** 68k nps is the binding constraint. Build with
   `-O3 -march=native`, real magic bitboards (the current path falls back to slow
   loop attacks), and staged move generation.
2. **Eval magnitude calibration.** Scale/retune so that a -400cp position reads
   near -400, not -140. Fit the scaling factor against SF18-labelled positions
   (outcome data only, no SF code).
3. **Middlegame defence vs forcing lines.** 65 of 97 blunders and all 20
   mate-walks are here; extend/verify on checks and near-mate lines.
4. **Re-test PSQT with 200+ games** — 16 games cannot resolve a ±50 Elo change.
5. **Opening book** to sidestep the weak opening eval entirely.
6. **Endgame steering** — the phase where SF18 is least sharp relative to us.

---

## 7. What was built

| file | purpose |
|---|---|
| `lab/harness.py` | plays NextGen vs SF18 across 16 openings, both colours, configurable SF Elo/TC; writes PGN + per-ply JSONL (eval, depth, seldepth, nodes, nps, pv, time) |
| `lab/analyze.py` | SF18-referenced blunder detection, per-phase cp-loss, per-piece blunder counts, SF move-style profiling |
| `lab/explain.py` | **why a move is best / when to play it**, rule-based (captures, checks, development, centre, threats, hanging pieces) + why our move was worse; emits coaching text and JSONL training labels |
| `lab/sprt.py` | SPRT + Wilson CI + Elo conversion — the acceptance gate |
| `lab/loop.py` | the full cycle: play → log → analyse → build candidate → A/B → accept only on significance → repeat |

All learning is from **game outcomes, PGNs and our own analysis**. No Stockfish
source was read, copied or derived from — SF18 is used purely as an opponent
and as an oracle for position labels.
