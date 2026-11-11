@echo off
REM ============================================================
REM  Launcher for dhewm3 - DOOM 3 source port
REM  Build: VS2022 x64 RelWithDebInfo
REM  Usage: double-click, or pass extra args, e.g.
REM         launch-dhewm3.bat +map game/mars_city1
REM         launch-dhewm3.bat +set r_fullscreen 0
REM  Singleplayer only: Doom 3 and source-port multiplayer
REM  netcode is insecure - do not play on public servers.
REM ============================================================
setlocal

set "BIN_DIR=%~dp0build\main\RelWithDebInfo"
set "EXE=%BIN_DIR%\dhewm3.exe"
set "GAMEDATA=C:\Program Files (x86)\Steam\steamapps\common\Doom 3"

if not exist "%EXE%" goto :noexe
if not exist "%GAMEDATA%\base\pak000.pk4" goto :nodata
goto :run

:noexe
echo [ERROR] Cannot find dhewm3.exe under "%BIN_DIR%"
echo Build it first: cmake --build ... --config RelWithDebInfo
goto :fail

:nodata
echo [ERROR] Missing game data: "%GAMEDATA%\base\pak000.pk4"
echo Install DOOM 3 v1.3.1 from Steam, NOT the BFG Edition.
echo Or edit GAMEDATA in this file to your install path.
goto :fail

:run
pushd "%BIN_DIR%"
"%EXE%" +set fs_basepath "%GAMEDATA%" +set r_fullscreen 0 %*
set "RC=%ERRORLEVEL%"
popd
if "%RC%"=="0" goto :done
echo [dhewm3 exited with code %RC%]
goto :fail

:done
endlocal
exit /b 0

:fail
pause
endlocal
exit /b 1
