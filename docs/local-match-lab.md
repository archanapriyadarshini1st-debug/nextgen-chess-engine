# NextGen Local Match Lab (Windows)

The local Match Lab runs NextGen against Stockfish 19 MAX on a Windows PC. It is designed for resumable research sessions and stores each completed game atomically.

## Requirements

- Windows 10/11
- Python 3.10 or newer
- `python -m pip install --user python-chess`
- A built NextGen executable at `bin/chess_engine.exe`
- Stockfish 19 Windows executable under `bin/stockfish19/`

The runner searches Stockfish recursively under `bin/stockfish19/`, so the official ZIP's nested folder layout is supported.

## Choose a time control

At the repository root, double-click `Start-Match-Lab.bat` to open the clickable time-control picker. Choose a preset or enter a custom value such as `7+2`.

Available presets:

- Bullet: `1+0`, `2+1`
- Blitz: `3+0`, `3+2`, `5+0`, `5+3`
- Rapid: `10+0`, `10+5`, `15+10`

The format is **base minutes + increment seconds**. You can also start directly from a terminal:

```bat
Start-Match-Lab.bat 5+3
```

Or have the terminal prompt for a value:

```bat
Start-Match-Lab.bat --terminal
```

The Python runner can be called directly:

```powershell
python tools/local_match_lab.py --time-control 3+0
```

Validate a selection without starting either engine:

```powershell
python tools/local_match_lab.py --time-control 5+3 --validate-only
```

The default mode runs continuously. Add `--max-games 2` for a bounded run. Stop after the current game by creating `STOP_AFTER_GAME` in the repository root, or press Ctrl+C in the console. Completed games are persisted before the next game starts. Opening selection and NextGen's White/Black alternation are tracked independently for each time control, so switching from one clock to another does not split a paired opening test.

## Real clock and fair resources

The selected time control is a real UCI clock, not a fixed time-per-move budget. Both engines receive White's and Black's remaining clock values on every move. The runner tracks elapsed time independently for both sides, applies the selected Fischer increment, records the clock trace, and scores flag falls correctly.

Both engines use one thread and 64 MB Hash on the target 8 GB laptop. Stockfish is configured with `UCI_LimitStrength=false`; no Elo or skill cap is applied. The NextGen experience book is disabled so the match measures the current engine baseline rather than memorized opening hints. The 600-ply cap is a high safety limit, not a normal game-ending target.

## Teacher labels

The runner labels every fourth NextGen move by default. For each sampled position, Stockfish 19 gets a 3-second search for its best move and a separate 3-second search constrained to the move NextGen actually played. The resulting record includes the requested time budget and achieved search depth. This is substantially more deliberate than the original 350 ms sampling setup, but an engine evaluation remains an estimate rather than mathematical proof.

The old 350 ms labels are archived under `data/legacy/` and excluded from the clean active teacher files. Their position IDs remain marked complete so the runner does not spend time silently relabelling the old games at startup.

## Resumability and outputs

Every completed game is first written as an atomic JSON record under `data/game_records/`. Combined PGN and JSONL files are rebuilt from canonical records on startup if a prior shutdown interrupted an export.

- `data/games.pgn`: all completed games
- `data/games.jsonl`: game summaries and per-move clock trace
- `data/positions.jsonl`: positions where NextGen moved, each tagged with the selected time control
- `data/teacher_positions.jsonl`: clean Stockfish evaluations and best moves for sampled positions, tagged with time control
- `data/missed_best_moves.jsonl`: sampled moves with a material evaluation loss, tagged with time control
- `data/legacy/`: preserved fast preliminary labels and missed-move records
- `reports/session_summary.json`: aggregate counts and results grouped by selected time control

Game records store the Chess.com-style label (for example, `3+0`) separately from the PGN-standard `TimeControl` value, which is expressed in seconds.

A Windows file lock prevents two Match Lab sessions from writing to the same dataset concurrently. The stop flag is cleared only after the new process has acquired that lock.

## Learning process

Played moves are not assumed to be correct. Stockfish labels are written to a separate teacher dataset; the runner does not automatically replace the NNUE network.

When there are at least 100 usable teacher positions, copy the clean teacher files into the repository's `datasets/` folder and train a separate candidate network with the existing trainer:

```powershell
python training/train_nnue_teacher.py --dataset datasets/teacher_positions.jsonl --dataset datasets/missed_best_moves.jsonl --out networks/local_match_candidate.pt --report networks/local_match_candidate_report.json --epochs 20 --patience 4 --cpu
```

A candidate must still pass the repository's independent evaluation and balanced engine match before replacing the baseline. Training loss or a lower validation MAE alone is not proof of higher playing strength.

## Interpreting results

The earlier 150 ms-per-move smoke-test games are useful only for checking that the pipeline runs; they are not calibrated rating evidence and should not be mixed with 3+0 tournament results when estimating strength. Reports keep results grouped by time control.

## Resumable tournament series

The first controlled pilot is **10 tournaments x 100 games = 1,000 games** at 3+0. Each tournament is 100 games. Adjacent games repeat the same opening with NextGen colors reversed, and later tournaments rotate to different openings.

On the Windows Match Lab PC, validate the defaults and then start the series:

    python tools/run_tournament_series.py --validate-only
    python tools/run_tournament_series.py

The defaults are 10 tournaments, 100 games per tournament, 3+0, one thread and 64 MB Hash per engine. Stockfish 19 runs uncapped. The local Match Lab labels every fourth NextGen move using the configured Stockfish analysis budget.

This is a **frozen-engine collection experiment**: no training or NNUE promotion occurs during the games. The NextGen experience book stays disabled. SHA-256 fingerprints of both engine binaries are saved so a changed binary cannot silently be mixed into the same series.

The first run prints a series ID. Keep it to resume after a pause or restart:

    python tools/run_tournament_series.py --series-id YOUR_PRINTED_SERIES_ID

Use the same settings when resuming. Stop-After-Game.bat safely stops after the active game; completed records remain saved. The manager stores per-series manifests and summaries under reports/tournament_series/. Canonical games, PGNs, positions and teacher labels stay in data/ and are tagged with their series and tournament IDs. Previous smoke-test games and archived fast labels are not included in this series summary.

The 1,000 games are a pilot dataset, not a strength claim. Keep the engine version fixed for the full baseline series; analyse and train candidates only after this collection is complete.