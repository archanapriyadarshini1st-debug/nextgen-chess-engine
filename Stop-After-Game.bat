@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if not exist STOP_AFTER_GAME (
  type nul > STOP_AFTER_GAME
)

echo Stop requested. The current game and its teacher labels will be saved.
echo The tournament series will pause and can be resumed with the same --series-id.
