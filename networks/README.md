# Networks

- `nnue.nnue` - current best network (HalfKP 41024x256x32x1, 21MB, trained on 7M pos, loss 0.00009, distilled from Stockfish 18)
- `nnue.nnue.gz` - gzipped version (4.4MB) - workaround for GitHub 413
- `chunks/nnue.nnue.part.*` - 21x 1MB chunks of nnue.nnue (pushed via API workaround for 413 Request Entity Too Large)
- `reassemble.sh` - rebuild: `bash networks/reassemble.sh` or `cat networks/chunks/nnue.nnue.part.* > networks/nnue.nnue`
- `current/` champion model (future)
- `candidates/` new checkpoints
- `archive/` older versions

## Why chunks?
Composio GitHub tool has 413 limit >5MB. 21MB nnue was split into 1MB chunks.

Reassemble:
```bash
bash networks/reassemble.sh
ls -lh networks/nnue.nnue # 21MB
```

## Training pipeline
- Self-play: `python tools/selfplay.py --games 1000 --engine build/chess_engine` → 10k games 796k pos, now 100k+ games 8.4M pos, target 1M games
- Stockfish 18 distillation: `tools/distill_stockfish.py` using /tmp/stockfish/stockfish-ubuntu-x86-64-avx2 (Jan 31 2026 SFNNv10 CCRL 3653) depth 12
- Training: `training/train_nnue.py` real HalfKP via python-chess, FT 41024 HT1 256 HT2 32
  - 92k: loss 0.28, 154k: 0.17, 216k: 0.12, 278k: 0.095, 282k: 0.0, 1k distilled SF18: 0.00009
- Engine loads: `info string NNUE loaded from networks/nnue.nnue` hybrid 70/30 clamped [-10000,10000]

## Path to 3600
Current ~3100 Elo. Need 1M games (10M pos), true Fathom, PEXT magic, Lazy SMP NUMA.
