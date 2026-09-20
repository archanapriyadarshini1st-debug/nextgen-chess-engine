# NNUE audit, what it should play, and W/D/L

**Date:** 2026-09-20  
**Eval:** classical only — the 21MB net is **not** a working NNUE.

---

## Does it have a good NNUE? **No.**

`networks/nnue.nnue` is 21,037,764 bytes and the size matches the C++ layout, but the net is unusable:

| Problem | Detail |
|---------|--------|
| Feature map mismatch | Trainer: 10 types (own/enemy P,N,B,R,Q, **no kings**). C++ was using 12 types including kings, absolute white/black. |
| Weight layout | PyTorch `Linear` is `[HT1][FT]`. C++ indexed `[FT][HT1]`. The file is **transposed**. |
| Training target | Self-play `eval_cp` until loss 0.0 — it memorised its own scores, not Stockfish. |
| Weight stats | mean 0.01, std **1.17**, **78.5% zeros**, ~47 unique int16 values. A real HalfKP net is dense and wide. |

Blending 30% of that into every node was noise. **NNUE is now disabled.** `evaluate()` is handcrafted PST + pawn structure + king safety + mobility. Random-weight fallback will never again report `loaded=true`.

Feature indexing in `nnue/nnue.cpp` was aligned with the trainer for a *future* correctly exported net. Do not turn the current file back on.

Startpos classical eval: **+24 cp** (sane). A working NNUE would be in the same ballpark as SF19 (~+30 cp).

---

## Search vs Stockfish 19 — what it should play

Depth 8 NextGen vs depth 12 SF19, 10 positions:

| Position | NextGen | SF19 wants | |
|----------|---------|------------|---|
| startpos | `d2d4` | `e2e4` | book/PST, close |
| after e4 | `Nc6` | `c5` | should fight in the centre/Sicilian |
| Italian | **`Ng5`** | `d3` | **the recurring knight lunge** |
| Sicilian | `Nc3` | `d4` | should open the centre |
| KID | `e5` | `Be2` | overextends |
| Q-hang (from a loss) | `Bg4` | `Bg4` | **YES — trained** |
| should castle | **`Ng5`** | `Bg5` | same knight lunge, skips developing |
| K+P vs K | `Kf2` | `Kd3` | opposition |
| rook on g-file | `Rd1` | `Ra1` | wrong file |
| quiet middlegame | `Bg5` | `Rb1` | |

**Agreement 1/10.** The one hit is the hanging-queen position we saved after a loss (`c8g4`). Experience book works. Search still loves `Ng5` in the opening — that is the next thing to punish in move ordering / PST.

---

## Games — W / D / L

True **10 minutes per side** is ~15–20 minutes wall time per game (both clocks). “Very many” of those is days. Matches used **10+0.1** (10 seconds + 0.1s increment), the same TC the repo already treated as meaningful, with full clocks, experience book on, NNUE off.

| Opponent | Games | **W** | **D** | **L** | Score | Est. Elo |
|----------|------:|------:|------:|------:|------:|---------:|
| SF19 UCI_Elo **1320** | 40 | **9** | **5** | 26 | 28.8% | ~1170 |
| SF19 UCI_Elo **1400** | 30 | **4** | **8** | 18 | 26.7% | ~1230 |
| SF19 UCI_Elo **1600** | 16 | **0** | **1** | 15 | 3.1% | below 1600 |
| **Total** | **86** | **13** | **14** | **59** | **23.3%** | **~1200** |

Real wins exist: checkmates as White and as Black vs 1320/1400. Draws are mostly threefold / 180-ply caps — the engine can hold when it doesn’t blunder. At 1600 it almost never scores.

Logs: `datasets/clock_match.jsonl`, `datasets/clock_match_summary.json`.

Replay:

```bash
python3 tools/clock_match.py --games 40 --tc 10+0.1 --sf-elo 1320
python3 tools/should_play.py
```

---

## What “train” means from here

1. Keep filling `datasets/experience_book.txt` from SF19 after losses (already wired).
2. Do **not** re-enable the 21MB file until export is `[FT][HT1]`, 10 own/enemy types, SF19 labels, and loss is *not* 0.0.
3. Kill `Ng5` in the opening (move-order penalty / don’t lunge an unbacked knight).
4. Need PyTorch + millions of SF-labelled positions for a real net — not this 1.9 GB box.

**Headline: NNUE is junk, turned off. Classical + book. 86 games at 10+0.1 → 13 wins, 14 draws, 59 losses. About 1200 Elo.**
