
# NextGen Chess Engine

A modular chess engine in modern C++ with Python tooling for self-play, training, benchmarking, and analytics.

## Built so far
- Board representation and legal move generation
- Zobrist hashing
- Alpha-beta / negamax search with quiescence, null move pruning, LMR, aspiration windows, and TT
- UCI interface
- Regression tests
- Basic multi-threaded root search
- Opening-book hook and Syzygy hook
- Python scaffolding for logging, training, and benchmarking

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

## UCI options
- `Hash`
- `Threads`

## Next major work
- True NNUE feature extractor and trainer
- Real Syzygy probing via an external probe library
- Stronger move ordering with SEE integration
- Better endgame evaluation and draw detection
- Stockfish/Lc0 tournament harness and SPRT loops
