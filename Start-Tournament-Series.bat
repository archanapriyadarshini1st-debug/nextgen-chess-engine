@echo off
setlocal EnableExtensions
cd /d "%~dp0"

python -c "import chess" >nul 2>&1
if errorlevel 1 (
  echo Installing python-chess...
  python -m pip install --user python-chess
  if errorlevel 1 (
    echo Could not install python-chess.
    pause
    exit /b 1
  )
)

echo.
echo NextGen Tournament Series
echo Default pilot: 10 tournaments x 100 games = 1,000 games
echo Default clock: 3+0 ^| Stockfish 19 MAX ^| frozen engine binaries
echo Use Stop-After-Game.bat to pause safely, then resume with the printed series ID.
echo.
python tools\run_tournament_series.py %*
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (
  echo Tournament series completed.
) else if "%RESULT%"=="3" (
  echo Tournament series paused. Resume with the same --series-id.
) else (
  echo Tournament series ended with exit code %RESULT%.
)
pause
exit /b %RESULT%
