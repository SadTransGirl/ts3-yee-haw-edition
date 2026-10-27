@echo off
setlocal
rem One-PC online co-op test: starts two copies of the game that play together
rem over this PC's loopback, exactly like two PCs would over Steam.
rem   Window "PLAYER 1 (host)"   = controller 1
rem   Window "PLAYER 2 (client)" = controller 2
rem With only ONE controller connected, both windows share it: it starts on
rem player 1, and Back + RB hands it to player 2 and back (the other player
rem stands still).
rem Steam must be running. Your real save is only read, never changed:
rem player 2 plays from a copy kept under %%APPDATA%%\TS3 local co-op test.
rem
rem Options in xinput_harness.ini, [netplay] section (all optional):
rem   local_test_host_pad=0        which controller is player 1
rem   local_test_client_pad=1      which controller is player 2
rem   local_test_controllers=auto  1 = always share one controller, 2 = always two
rem   local_test_client=bot        drive player 2 with random presses instead
rem   local_test_latency_ms=60     add network delay, to feel online play
rem
rem Drop-in test:  local_coop_test.cmd dropin
rem   Starts only player 1 first. Play into a level (for example Toy Box), then
rem   press a key in this window: player 2 starts and joins the game in progress
rem   (player 1 saves, both reload that world together, player 2 presses Start).

set GAME=%~dp0..
set REAL=%APPDATA%\Disney\Toy Story 3 Complete Edition\savedata
set P2=%APPDATA%\TS3 local co-op test\player 2\Disney\Toy Story 3 Complete Edition\savedata

if not exist "%GAME%\xinput1_4.dll" (
    echo The harness DLL isn't installed in the game folder. Run build.cmd first.
    exit /b 1
)
tasklist /fi "imagename eq Toy Story 3 Complete Edition.exe" | find /i "Toy Story 3" >nul
if not errorlevel 1 (
    echo Toy Story 3 is already running. Close it first.
    exit /b 1
)

rem Player 2 starts from a fresh copy of your save. Its own screen settings
rem (newsettings.bin) are kept after the first run, so you can give that
rem window a different size in the game's options.
if not exist "%P2%" mkdir "%P2%"
robocopy "%REAL%" "%P2%" /MIR /XF newsettings.bin *.bak /NFL /NDL /NJH /NJS /NP >nul
if not exist "%P2%\newsettings.bin" if exist "%REAL%\newsettings.bin" copy /y "%REAL%\newsettings.bin" "%P2%\newsettings.bin" >nul

set SteamAppId=4049180
set SteamGameId=4049180
echo Starting player 1 (host)...
start "" /D "%GAME%" "%GAME%\Toy Story 3 Complete Edition.exe" +tsnet_local host
if /i "%~1"=="dropin" goto dropin
timeout /t 6 /nobreak >nul
echo Starting player 2 (client)...
start "" /D "%GAME%" "%GAME%\Toy Story 3 Complete Edition.exe" +tsnet_local client
echo.
echo Both copies are starting. When both windows show "Connected",
echo hold Back + Start on both controllers to begin.
echo With one controller: player 1 drives the menus. At the character
echo selection (or in the level) press Back + RB to hand the controller to
echo player 2's window, then Start there to join as player 2.
echo Logs: harness\runs\local_host and harness\runs\local_client
exit /b 0

:dropin
echo.
echo Drop-in test. In player 1's window: load your game and play into a level
echo (Toy Box is a good one). Take your time.
echo When you're in the level, come back here and press a key to bring player 2 in.
pause >nul
echo Starting player 2 (client): it joins the game in progress...
start "" /D "%GAME%" "%GAME%\Toy Story 3 Complete Edition.exe" +tsnet_local client
echo.
echo Player 1's game saves, returns to the title for a moment, and both load that
echo world together. When the banner says so, press Start for player 2.
echo With one controller: Back + RB hands it to player 2 first.
echo Logs: harness\runs\local_host and harness\runs\local_client
exit /b 0
