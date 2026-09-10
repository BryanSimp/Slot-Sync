@echo off
REM Start Dolphin with the memory card already up to date.
REM
REM Use this instead of Dolphin.exe. It pulls whatever the consoles pushed,
REM points Dolphin at the card, and only then starts the emulator -- so there is
REM no window in which you can begin playing against a card the server has
REM already moved past. Saving from a stale card forks the save, and that fork
REM has to be settled by hand in the web UI.
REM
REM With no argument it syncs whichever game Dolphin is currently set up for.
REM Pass a game id to switch: slotsync-dolphin.cmd GC6E01
REM
REM Dolphin must be closed. It rewrites Dolphin.ini from the settings it loaded
REM at startup, so a card switched underneath a running instance is reverted.

setlocal
cd /d "%~dp0"
set PYTHONPATH=src

python -m slotsync_dolphin launch %1
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Did not start Dolphin. Nothing was changed.
    pause
)
endlocal
