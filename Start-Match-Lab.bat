@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if not exist bin\chess_engine.exe (
  echo NextGen engine binary is missing from bin\
  pause
  exit /b 1
)
if not exist bin\stockfish19\stockfish\stockfish-windows-x86-64-universal.exe (
  echo Searching for Stockfish 19...
  dir /s /b bin\stockfish19\*.exe
)
python -c "import chess" >nul 2>&1
if errorlevel 1 (
  echo Installing python-chess...
  python -m pip install --user python-chess
)

set "TIME_CONTROL=%~1"
if /i "%TIME_CONTROL%"=="--terminal" (
  set /p "TIME_CONTROL=Enter time control (minutes+increment seconds, e.g. 3+0 or 5+3): "
) else if "%TIME_CONTROL%"=="" (
  set "TC_FILE=%TEMP%\nextgen-time-control-%RANDOM%-%RANDOM%.txt"
  powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "tools\select_time_control.ps1" -OutputFile "%TC_FILE%"
  if errorlevel 1 (
    echo Time-control selection cancelled or failed.
    del "%TC_FILE%" >nul 2>&1
    pause
    exit /b 1
  )
  if not exist "%TC_FILE%" (
    echo Time-control picker did not return a selection.
    pause
    exit /b 1
  )
  set /p "TIME_CONTROL="<"%TC_FILE%"
  del "%TC_FILE%" >nul 2>&1
)

echo.
echo NextGen Local Match Lab
echo Selected time control: %TIME_CONTROL%
echo Stockfish 19 MAX. Completed games auto-save.
echo Teacher: Stockfish gets 3 seconds per analysis, every 4th NextGen move.
echo Use Stop-After-Game.bat to stop after the current game.
echo.
python tools\local_match_lab.py --time-control "%TIME_CONTROL%" --hash-mb 64 --threads 1 --teacher-every 4 --teacher-seconds 3.0 --max-ply 600
echo.
echo Match Lab stopped. Press any key to close.
pause >nul
