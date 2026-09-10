@echo off
REM Push GameCube memory cards to the SlotSync server as Dolphin writes them.
REM
REM Leave this running while you play. It polls the cards directory, waits for a
REM card to stop changing, and pushes it -- so an in-game save reaches the server
REM without quitting the emulator, the same way the Nintendont kernel client does
REM it on the console.
REM
REM It never overwrites: if another device moved the card on while you were
REM playing, the push is refused as a conflict, logged, and your card is left
REM alone for you to settle in the web UI.
REM
REM Run `setup` once first, or this has no server to talk to. Ctrl-C to stop.

setlocal
cd /d "%~dp0"
set PYTHONPATH=src

python -m slotsync_dolphin watch
set RC=%ERRORLEVEL%

REM Ctrl-C leaves 2 or 130 depending on the shell; neither is a failure worth
REM holding the window open for.
if %RC% NEQ 0 if %RC% NEQ 2 if %RC% NEQ 130 (
    echo.
    echo slotsync-dolphin watch exited with %RC%.
    pause
)
endlocal
