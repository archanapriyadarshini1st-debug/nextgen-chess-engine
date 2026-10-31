# NextGen Local Match Lab (Windows)

The local Match Lab runs the NextGen UCI engine against Stockfish 19 MAX on a Windows PC. It is designed for manually started, resumable research sessions.

## Requirements

- Windows 10/11
- Python 3.10 or newer
- `python -m pip install --user python-chess`
- A built NextGen executable at `bin/chess_engine.exe`
- Stockfish 19 Windows executable under `bin/stockfish19/`

The runner searches Stockfish recursively under `bin/stockfish19/`, so the official ZIP's nested folder layout is supported.

## Run

From the repository root:

```powershell
python tools/local_match_lab.py --think-seconds 0.15 --hash-mb 64 --threads 1 --teacher-every 4 --teacher-seconds 0.35 --max-ply 180
```

The default mode runs continuously. Add `--max-games 2` for a bounded smoke test. Stop after the current game by creating `STOP_AFTER_GAME` in the repository root, or press Ctrl+C in the console. Completed games are persisted before the next game starts.

## Fair resource settings

Both engines receive the same per-move time, hash and thread count. Stockfish is configured with `UCI_LimitStrength=false`; no Elo or skill cap is applied. The NextGen experience book is disabled so the match measures the search/evaluation baseline rather than memorized opening hints.

## Resumability and outputs

Every completed game is first written as an atomic JSON record under `data/game_records/`. The combined PGN and JSONL files are append-only during the run and rebuilt from canonical records on startup if a prior shutdown interrupted an export.

- `data/games.pgn`: all completed games
- `data/games.jsonl`: game summaries
- `data/positions.jsonl`: positions where NextGen moved
- `data/teacher_positions.jsonl`: Stockfish evaluations and best moves for sampled positions
- `data/missed_best_moves.jsonl`: sampled moves with a material evaluation loss
- `reports/session_summary.json`: aggregate counts and resource settings

A Windows file lock prevents two Match Lab sessions from writing to the same dataset concurrently. The stop flag is cleared only after the new process has acquired that lock.

## Learning process

The runner labels every Nth NextGen move (4 by default) using Stockfish 19 analysis. It evaluates both Stockfish's best move and the move NextGen actually played. It does **not** assume the played move is correct and it does not automatically replace the NNUE network.

When there are at least 100 usable teacher positions, copy the exported teacher files into the repository's `datasets/` folder and train a separate candidate network with the existing trainer:

```powershell
python training/train_nnue_teacher.py --dataset datasets/teacher_positions.jsonl --dataset datasets/missed_best_moves.jsonl --out networks/local_match_candidate.pt --report networks/local_match_candidate_report.json --epochs 20 --patience 4 --cpu
```

A candidate must still pass the repository's independent evaluation and balanced engine match before replacing the baseline. Training loss or a lower validation MAE alone is not proof of higher playing strength.

## Important interpretation

These fast local self-play/teacher games are for finding tactical weaknesses and growing a labelled dataset. Do not use a short 150 ms per-move match as a calibrated Elo test. Use the separate balanced clock-match workflow for strength measurement.
