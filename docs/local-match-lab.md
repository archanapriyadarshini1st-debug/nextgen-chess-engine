# NextGen Local Match Lab (Windows)

The local Match Lab runs NextGen against Stockfish 19 MAX on a Windows PC. It is designed for resumable research sessions and stores each completed game atomically.

## Requirements

- Windows 10/11
- Python 3.10 or newer
- `python -m pip install --user python-chess`
- A built NextGen executable at `bin/chess_engine.exe`
- Stockfish 19 Windows executable under `bin/stockfish19/`

The runner searches Stockfish recursively under `bin/stockfish19/`, so the official ZIP's nested folder layout is supported.

## Run

Double-click `Start-Match-Lab.bat`, or run from the repository root:

```powershell
python tools/local_match_lab.py --clock-seconds 180 --increment-seconds 0 --hash-mb 64 --threads 1 --teacher-every 4 --teacher-seconds 3.0 --max-ply 600
```

The default mode runs continuously. Add `--max-games 2` for a bounded test. Stop after the current game by creating `STOP_AFTER_GAME` in the repository root, or press Ctrl+C in the console. Completed games are persisted before the next game starts.

## Tournament clock and fair resources

Both engines receive an actual UCI clock of **3 minutes each, with no increment (3+0)**. The runner sends White's and Black's current remaining clock values to the engine on every move, tracks elapsed time independently for each side, adds any configured Fischer increment, and records clock usage in each game. A flag fall is scored as a time forfeit, with the standard insufficient-mating-material draw condition handled.

Both engines use one thread and 64 MB Hash on the target 8 GB laptop. Stockfish is configured with `UCI_LimitStrength=false`; no Elo or skill cap is applied. The NextGen experience book is disabled so the match measures the current engine baseline rather than memorized opening hints. The 600-ply cap is a high safety limit, not a normal game-ending target.

## Teacher labels

The runner labels every fourth NextGen move by default. For each sampled position, Stockfish 19 gets a 3-second search for its best move and a separate 3-second search constrained to the move NextGen actually played. The resulting record includes the requested time budget and achieved search depth. This is substantially more deliberate than the original 350 ms sampling setup, but an engine evaluation remains an estimate rather than mathematical proof.

## Resumability and outputs

Every completed game is first written as an atomic JSON record under `data/game_records/`. Combined PGN and JSONL files are rebuilt from canonical records on startup if a prior shutdown interrupted an export.

- `data/games.pgn`: all completed games
- `data/games.jsonl`: game summaries and per-move clock trace
- `data/positions.jsonl`: positions where NextGen moved
- `data/teacher_positions.jsonl`: Stockfish evaluations and best moves for sampled positions
- `data/missed_best_moves.jsonl`: sampled moves with a material evaluation loss
- `reports/session_summary.json`: aggregate counts and resource/time-control settings

A Windows file lock prevents two Match Lab sessions from writing to the same dataset concurrently. The stop flag is cleared only after the new process has acquired that lock.

## Learning process

Played moves are not assumed to be correct. Stockfish labels are written to a separate teacher dataset; the runner does not automatically replace the NNUE network.

When there are at least 100 usable teacher positions, copy the exported teacher files into the repository's `datasets/` folder and train a separate candidate network with the existing trainer:

```powershell
python training/train_nnue_teacher.py --dataset datasets/teacher_positions.jsonl --dataset datasets/missed_best_moves.jsonl --out networks/local_match_candidate.pt --report networks/local_match_candidate_report.json --epochs 20 --patience 4 --cpu
```

A candidate must still pass the repository's independent evaluation and balanced engine match before replacing the baseline. Training loss or a lower validation MAE alone is not proof of higher playing strength.

## Interpreting results

The earlier 150 ms-per-move smoke-test games are useful only for checking that the pipeline runs; they are not calibrated rating evidence and should not be mixed with 3+0 tournament results when estimating strength. Keep datasets and evaluation reports tagged by time control.
