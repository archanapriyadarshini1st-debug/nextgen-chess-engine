
# NextGen Chess Engine

A modular chess engine in modern C++ with Python tooling for self-play, training, benchmarking, and analytics.

## What is in the repository now
- Bitboard-friendly board representation
- Legal move generation, castling, en passant, promotions
- Zobrist hashing
- Alpha-beta / negamax search with quiescence, aspiration windows, null move pruning, LMR, and TT
- UCI interface and perft-style regression tests
- Python scaffolding for self-play, datasets, training, and benchmarks

## Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Run
```bash
./build/chess_engine
```

Then send UCI commands such as:
```text
uci
isready
position startpos moves e2e4 e7e5
go depth 4
```

## Next upgrades
- Better move ordering tables and transposition aging
- Incremental NNUE feature accumulators
- Syzygy probing
- Multi-threaded search and time management refinements
- Self-play dataset generation and network training
