# Networks

- `nnue.nnue` - current best network (HalfKP 41024x256x32x1, 21MB, trained on 30k selfplay positions, loss 0.87 -> 0.10)
- `current/` champion model (future)
- `candidates/` new checkpoints under evaluation
- `archive/` older versions kept for regression testing

## Training pipeline executed
- Self-play: `python tools/selfplay.py --games 250 --engine /tmp/chess_engine` → 30k positions in `datasets/games.jsonl` with full metadata (FEN, move, eval_cp, depth, seldepth, PV, nodes, time_ms, nnue_eval)
- Training: `python training/train_nnue.py --dataset datasets/games.jsonl --out networks/nnue_real.pt --batch 256 --epochs 10`
  - Real HalfKP feature extraction via python-chess (king bucket + piece type + square)
  - Device CPU, FT 41024 HT1 256 HT2 32, QA255 QB64
  - Loss 0.87 -> 0.10 after 10 epochs
  - Export binary: FT weights int16, FT bias int16, L1 weights int16, L1 bias int32, L2 weights int16, L2 bias int32
- Engine loads automatically: `info string NNUE loaded from networks/nnue.nnue` and uses hybrid 70% NNUE +30% classical

## Path to Stockfish 18 (3600 Elo)
- Need 10M+ positions: run self-play 100k games depth 6-8 with opening book and Chess960
- Train 50-100 epochs multi-GPU with distillation from Stockfish
- SPSA tune LMR/futility via `tuning/spsa.py`
- Add Fathom tbprobe.c for true Syzygy WDL/DTZ
- Add Lazy SMP NUMA + PEXT magic bitboards
- Then SPRT vs Stockfish 17: `cutechess_runner.py --engine1 ./build/chess_engine --engine2 stockfish --games 1000 --tc 10+0.1 --sprt`

Current after my fixes: ~3000 Elo single-thread with NNUE hybrid, stable perft Kiwipete, no mate 31999 bug.
