# NextGen Chess Engine

A modular chess engine foundation in modern C++ with Python tooling for self-play, training, benchmarking, and analytics.

## Goals
- UCI-compatible engine core
- Bitboard-based move generation
- Iterative deepening alpha-beta search
- NNUE-ready evaluation pipeline
- Self-play and training tooling in Python
- Benchmarking, tuning, and logging infrastructure

## Layout
- `core/` fundamental chess types, board representation, move generation, hashing
- `search/` iterative deepening, pruning, move ordering, transposition table
- `evaluation/` classical and endgame evaluation helpers
- `nnue/` incremental neural evaluation interfaces
- `uci/` protocol loop and options
- `tests/` perft and correctness tests
- `tools/` Python training, dataset, and benchmark scripts
- `training/` model training and orchestration

## Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Roadmap
1. Legal move generation and perft validation
2. UCI shell and time management
3. Transposition table + search heuristics
4. NNUE integration
5. Self-play logging and training pipeline
6. Automated regression testing and benchmarking
