# NextGen Chess Memory (Chrome MV3)

This is a local companion for the NextGen C++ engine. It remembers positions and imported completed games, can ask the locally built engine to analyse a FEN, and exports a real folder of PGNs and JSONL data.

## Fair-play boundary

The extension does **not** read an active Chess.com board, choose moves for a live game, click squares, or autoplay. Use it for positions you enter yourself and PGNs copied from games that have finished. The analysis button only analyses the FEN you manually place in the lab.

## 1. Download and load the extension

1. Download the repository ZIP for this branch and extract it.
2. Open Chrome and visit `chrome://extensions`.
3. Turn on **Developer mode**.
4. Choose **Load unpacked** and select the repository's `browser-extension` folder.
5. Copy the extension ID shown on the extension card.

## 2. Build and connect the local NextGen engine (Windows)

From PowerShell, run this from the extracted repository:

```powershell
powershell -ExecutionPolicy Bypass -File .\browser-extension\install-native-host.ps1
```

Paste the extension ID when asked. The script builds `chess_engine.exe` from the repository's C++ source with CMake, compiles the Native Messaging bridge, and registers it for your Windows user account.

Requirements: CMake, a C++20 compiler (Visual Studio 2022 Build Tools with **Desktop development with C++** is a straightforward option), and the .NET Framework C# compiler. The script stops with an explicit error if one is missing.

After setup, open the extension and enter a FEN to use **Analyse with NextGen engine**. Depth is capped at 24, hash is set to 64 MB and the engine uses one thread by default. The experimental NNUE checkbox is off by default; enable it only when you deliberately want to test the current network.

## 3. Save completed Chess.com games

After a game has ended, open its analysis/review screen and copy the PGN. Open the extension, paste it into **Import a completed game**, give it a label, then add it to memory. You can also save a FEN manually in the Position lab.

The extension stores its working memory in Chrome's local extension storage. Nothing is uploaded to a server.

## 4. Export a folder of positions

Click **Create / update memory folder** and choose the parent folder on your computer. It creates:

```text
NextGen-Chess-Memory/
  games/
    all_games.pgn
    0001-game-label.pgn
  positions/
    manual_positions.jsonl
  analyses/
    engine_analyses.jsonl
  summary.json
  README.txt
```

To extract every legal position from imported PGNs, from the repository root run:

```powershell
py -m pip install python-chess
py scripts/build_extension_memory_dataset.py "C:\path\to\NextGen-Chess-Memory"
```

That creates `positions/positions.jsonl`. It records FENs and the moves actually played. This is an experience dataset, not a set of expert evaluations.

## 5. How the data can improve NextGen

A played move by itself is not a reliable training target. To train the evaluation network, positions need independent teacher scores. Use Stockfish to label the exported PGN with the repository's existing tool:

```powershell
py -m pip install python-chess numpy torch
py tools/label_teacher_positions.py --pgn "C:\path\to\NextGen-Chess-Memory\games\all_games.pgn" --missed "C:\path\to\NextGen-Chess-Memory\positions\missed_best_moves.jsonl" --stockfish "C:\path\to\stockfish.exe" --out "C:\path\to\NextGen-Chess-Memory\positions\teacher_labels.jsonl" --max-positions 500 --depth 10
```

Then train a **candidate** network (not an automatic replacement):

```powershell
py training/train_nnue_teacher.py --dataset "C:\path\to\NextGen-Chess-Memory\positions\teacher_labels.jsonl" --out networks\extension_candidate.pt --report networks\extension_candidate_report.json --epochs 20 --patience 4 --cpu
```

The trainer enforces a minimum of 100 usable teacher-labelled positions and a held-out validation split. A lower validation error does not prove a higher Elo. Keep the candidate separate, run the repository's independent candidate validation and a fresh balanced Stockfish match, and only then decide whether to adopt it.

## Data and privacy

- Data remains in Chrome local storage until you export it.
- Export uses the browser's folder picker and writes files only into the folder you choose.
- The extension has no website permissions and no Chess.com content script.
- Clear extension memory removes its local records; it does not delete previously exported files.
