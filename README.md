# NextGen Chess Engine

Modular C++20 UCI engine. **Measured Elo ≈ 1200** against Stockfish 19 skill levels. It is not Stockfish-class. Older notes claiming 2500–3100 Elo were not measured.

## Honest status (2026-09-20)

- Perft is correct (startpos 20/400/8902/197281, Kiwipete 48/2039/97862).
- Search no longer emits `bestmove 0000` when mated (that used to forfeit games).
- **NNUE is off.** The 21MB HalfKP file does not match the C++ feature map (and was trained on self-play until loss 0.0). Eval is classical PST + structure.
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
