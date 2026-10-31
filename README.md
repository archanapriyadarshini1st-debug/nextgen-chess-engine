# NextGen Chess Engine

Modular C++20 UCI engine. **Measured Elo ≈ 1200** against Stockfish 19 skill levels. It is not Stockfish-class. Older notes claiming 2500–3100 Elo were not measured.

## Honest status (2026-09-20)

- Perft is correct (startpos 20/400/8902/197281, Kiwipete 48/2039/97862).
- Search no longer emits `bestmove 0000` when mated (that used to forfeit games).
- **NNUE is opt-in only**: by default the engine uses classical evaluation. To compare a trained network, load it explicitly with UCI `setoption name NNUEFile value networks/nnue.nnue`. The evaluator must not silently auto-load a network just because a file happens to exist.
- The compact teacher-trained NNUE is experimental. A previous 40-game candidate report is **not a valid strength claim**: the validator allowed the shared Stockfish-labelled experience book in both engines, and the NNUE accumulator had a king-feature indexing bug. The experiment branch now fixes both issues; rerun the gate and the independent Stockfish benchmark before claiming strength gains.
- We do **not** call Stockfish from `evaluate()`. That was cheating and was removed.
- After every loss vs **Stockfish 19 full strength**, the engine saves the move SF19 would have played into `datasets/experience_book.txt` and loads it on `isready`.

### Measured vs Stockfish 19

| Match | W–D–L | Notes |
|-------|-------|-------|
| SF19 UCI_Elo 1320, 10+0.1, 40 games | 9–5–26 | real wins exist |
| SF19 UCI_Elo 1400, 10+0.1, 30 games | 4–8–18 | |
| SF19 **MAX**, 120 + more games | ~0–1–N | teacher for best-move labels |

Full write-ups: `ELO_SF19_REPORT.md`, `NNUE_AND_GAMES_REPORT.md`.

## Build

```bash
g++ -std=c++20 -O3 -march=native -pthread -I. \
  core/position.cpp core/movegen.cpp core/zobrist.cpp \
  search/search.cpp search/tt.cpp search/heuristics.cpp \
  evaluation/eval.cpp nnue/nnue.cpp \
  uci/uci.cpp logging/game_log.cpp tb/syzygy.cpp opening/book.cpp \
  main.cpp -o bin/chess_engine
```

Do **not** compile `evaluation/stockfish_nnue.cpp` — that forks Stockfish as the eval.

```
printf 'uci\nisready\nposition startpos\ngo depth 8\nquit\n' | ./bin/chess_engine
```

## Stockfish-supervised NNUE (experimental)

The manual GitHub Actions workflow `.github/workflows/sf19-max-training.yml` can run batches of 100, 250, 500, or 1,000 games against uncapped Stockfish 19. It uses equal 120 ms per move for both engines for the fast teacher-game batch, then analyses missed positions at Stockfish depth 8.

A batch can also train a compact NNUE from `datasets/missed_best_moves.jsonl`:

```bash
python -m pip install python-chess numpy torch
python training/train_nnue_teacher.py \
  --dataset datasets/missed_best_moves.jsonl \
  --out /tmp/nextgen_nnue.pt \
  --report networks/nnue_training_report.json \
  --epochs 20 --patience 4 --batch 128 --cpu
```

The trainer converts Stockfish labels to White's score perspective, holds out positions for validation, reports validation MAE/RMSE, and exports a binary matching the C++ feature map and fixed-point scales. CI includes a synthetic-network test for binary layout and score sign; the workflow also smoke-tests UCI loading before persisting the model and teacher data. The shipped old chunks under `networks/chunks/` are legacy and **must not be reassembled over the new compact model**.

This is an experimental evaluation model, not an Elo guarantee. The first 100-game baseline against Stockfish 19 MAX ended 0–100–0 (NextGen had 120 ms per move; Stockfish had 60 ms). That result is not a balanced tournament rating; it motivates further search/evaluation work. Strength must be measured separately with equal time controls and repeatable matches.

## Train vs Stockfish 19 MAX (the teacher)

```bash
# official SF19 binary somewhere on PATH or /tmp/sf19/stockfish19
python3 tools/sf19_max_train.py train --games 200 --our-ms 80 --sf-ms 40 --analyse-depth 8
```

This plays **uncapped** Stockfish 19, and after each loss writes:

- `datasets/missed_best_moves.jsonl` — FEN, our move, SF19 best, cp loss, why
- `datasets/experience_book.txt` — engine-readable best moves (auto-loaded)
- `datasets/games_vs_sf19.pgn`

Clock matches (not for training labels): `python3 tools/clock_match.py --games 40 --tc 10+0.1 --sf-elo 0` (`0` = SF19 max).

## UCI

`ExperienceFile`, `OwnBook`, `Hash`, `Threads`, `eval` (prints classical cp).

## Tests

```bash
g++ -std=c++20 -O2 -pthread -I. core/*.cpp search/*.cpp evaluation/eval.cpp nnue/nnue.cpp uci/uci.cpp logging/*.cpp tb/syzygy.cpp opening/*.cpp tests/test_main.cpp -o bin/engine_tests
./bin/engine_tests
```

## Evaluation integrity fixes (2026-10-17)

- NNUE loading is explicit, so a classical baseline cannot accidentally load a network from the working directory.
- NNUE accumulator refresh skips excluded king features instead of indexing before the feature array.
- Evaluation now scores actually controlled central squares, and a start-position side-to-move symmetry regression test guards against color bias.
- Elo/candidate measurements disable the shared experience book; otherwise both engines could replay teacher-suggested moves and hide the effect of their evaluators.
- Release-mode CTest keeps C++ assertions enabled. The short candidate gate is a screening test only; use the independent Stockfish tournament and a larger SPRT match for real strength claims.

## Local 1,000-game tournament pilot

On the Windows Match Lab PC, run `python tools/run_tournament_series.py --validate-only` and then `python tools/run_tournament_series.py` to run 10 paired-color tournaments of 100 games each (3+0 by default). The manager resumes by series ID, pins the NextGen and Stockfish binary hashes, tags every game and teacher label, and writes per-tournament and aggregate summaries under `reports/tournament_series/`. It does not train or replace an NNUE during this baseline collection. See `docs/local-match-lab.md`.