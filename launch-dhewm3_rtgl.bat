@echo off
REM ============================================================
REM  Launcher for dhewm3 with the RTGL1 (Vulkan path tracing) backend
REM  Branch    : rtgl-p0p1        Milestone: P0+P1 (empty-frame scaffolding)
REM
REM  Usage : double-click, or append engine args, e.g.
REM          launch-dhewm3_rtgl.bat +map game/mars_city1
REM          launch-dhewm3_rtgl.bat +set r_fullscreen 0
REM          launch-dhewm3_rtgl.bat +set r_rtgl_enable 0   (A/B: force GL)
REM
REM  NOTE  : P1 submits no geometry yet -> a stable BLACK screen is the
REM          expected PASS signal. Geometry / camera / lights are P2-P4.
REM  NOTE  : set D3RTGL_DRYRUN=1 to validate paths WITHOUT starting the game.
REM  NOTE  : override the RTGL build dir with D3RTGL_BIN, or the runtime
REM          folder with D3RTGL_DIR, instead of editing this file.
REM  NOTE  : DXGI present is the DEFAULT. On launch an "rt" junction is
REM          created beside dhewm3.exe so D3D12Core.dll can be found.
REM ============================================================
setlocal

REM --- RTGL-capable executable (built with -DD3_RTGL=ON) ---
set "RTGL_BIN=%~dp0build\rtgl-on\RelWithDebInfo"
if defined D3RTGL_BIN set "RTGL_BIN=%D3RTGL_BIN%"
set "RTGL_EXE=%RTGL_BIN%\dhewm3.exe"

REM --- stock build dir: supplies runtime DLLs missing from RTGL_BIN ---
set "STOCK_BIN=%~dp0build\main\RelWithDebInfo"

REM --- retail game data (DOOM 3 v1.3.1, NOT the BFG Edition) ---
set "GAMEDATA=C:\Program Files (x86)\Steam\steamapps\common\Doom 3"

REM --- RTGL1 runtime folder: bin\RTGL1.dll, shaders\*.spv, RTGL1.json ---
REM     Shared with gzdoom-rt; baked into the exe as r_rtgl_overridePath default.
REM     RTGL1 resolves shaders/JSON/DLL from this absolute override folder,
REM     NOT from CWD (VulkanDevice_Init.cpp / FolderObserver.cpp).
set "RTGL_DIR=I:\Sandbox\gzdoom-rt\build\rt"
if defined D3RTGL_DIR set "RTGL_DIR=%D3RTGL_DIR%"
set "RTGL_URI=%RTGL_DIR:\=/%"

REM --- pause only for interactive double-clicks, never in DRYRUN/scripts ---
set "DOPAUSE=" 
if not defined D3RTGL_DRYRUN if "%~1"=="" set "DOPAUSE=1"

REM --------------------------- validation ---------------------------
if not exist "%RTGL_EXE%" goto :noexe
if not exist "%GAMEDATA%\base\pak000.pk4" goto :nodata
if not exist "%RTGL_DIR%\bin\RTGL1.dll" goto :nortgl
dir /b "%RTGL_DIR%\shaders\*.spv" >nul 2>&1
if errorlevel 1 goto :noshaders
if not exist "%RTGL_BIN%\base.dll" goto :nodll
if not exist "%STOCK_BIN%\SDL2.dll" goto :nodll
if not exist "%STOCK_BIN%\OpenAL32.dll" goto :nodll
goto :info

:noexe
echo [ERROR] Cannot find the RTGL build: "%RTGL_EXE%"
echo         Build it first (branch rtgl-p0p1):
echo           cmake -A x64 -DDHEWM3LIBS=I:/Sandbox/dhewm3-libs/x86_64-w64-mingw32 -DD3_RTGL=ON -S I:/Sandbox/dhewm3/neo -B I:/Sandbox/dhewm3/build/rtgl-on
echo           cmake --build I:/Sandbox/dhewm3/build/rtgl-on --config RelWithDebInfo
echo         Or set D3RTGL_BIN to your RTGL RelWithDebInfo directory.
goto :fail

:nodata
echo [ERROR] Missing game data: "%GAMEDATA%\base\pak000.pk4"
echo         Install DOOM 3 v1.3.1 (NOT BFG), or edit GAMEDATA in this file.
goto :fail

:nortgl
echo [ERROR] Missing RTGL1.dll at "%RTGL_DIR%\bin\RTGL1.dll"
echo         Edit RTGL_DIR / set D3RTGL_DIR, or rebuild gzdoom-rt's RTGL1.
goto :fail

:noshaders
echo [ERROR] No compiled shaders in "%RTGL_DIR%\shaders"
echo         RTGL1.dll and the .spv files MUST come from the same build:
echo         a uniform-layout mismatch renders a black screen (only skybox).
goto :fail

:nodll
echo [ERROR] Missing runtime DLLs. Expected:
echo           "%RTGL_BIN%\base.dll" and d3xp.dll
echo           "%STOCK_BIN%\SDL2.dll" OpenAL32.dll libcurl-4.dll
echo         SDL/OpenAL/curl are added to PATH from STOCK_BIN at launch.
goto :fail

:info
echo [dhewm3 RTGL] exe      : %RTGL_EXE%
echo [dhewm3 RTGL] game data: %GAMEDATA%
echo [dhewm3 RTGL] runtime  : %RTGL_DIR%
findstr /R /I "developerMode.*true" "%RTGL_DIR%\RTGL1.json" >nul 2>&1
if not errorlevel 1 (
  echo [NOTE] RTGL1.json developerMode is ON: an ImGui debug window may
  echo        grab keyboard input and compete with the F10 settings menu.
  echo        Set it to false in "%RTGL_DIR%\RTGL1.json" if input feels stuck.
)
echo [NOTE] P1 expected result = stable BLACK screen + no Vulkan errors.
  echo [CONSOLE] win_viewlog 1: a dedicated Win32 console window opens (minimize, do NOT close).
  echo           rt_dbg_dumpLights 60 prints the per-light list there; type in that window:
  echo             set rt_dbg_dumpLights 0     (stop the list)
  echo             set rt_dbg_dumpLights 1     (every frame)
  echo           RTGL1 log spam is throttled by rt_dbg_printRepeat (default 300).
  echo [PRESENT] DXGI is the default (preferDxgiPresent=1); RTGL1 falls back
  echo           to native Vulkan automatically if D3D12/DXGI is unavailable.
echo.

if defined D3RTGL_DRYRUN (
  echo [DRYRUN] all path checks passed; would launch:
  echo   pushd "%RTGL_BIN%"
  echo   PATH prepend "%STOCK_BIN%"
  echo   "%RTGL_EXE%" +set r_rtgl_enable 1 +set r_rtgl_overridePath "%RTGL_URI%" +set fs_basepath "%GAMEDATA%" +set r_fullscreen 0 +set win_viewlog 1 +set rt_dbg_dumpLights 60 %*
  goto :done
)

:run
REM SDL2/OpenAL/libcurl are implicit imports: loader also searches PATH.
set "PATH=%STOCK_BIN%;%PATH%"
REM --- D3D12 Agility SDK: DXGI present loads D3D12Core.dll from "rt\bin\"
REM     (relative to the exe dir, exported by RG_D3D12CORE_HELPER). Mirror
REM     gzdoom-rt: junction <exe dir>\rt -> shared runtime folder.
if not exist "%RTGL_BIN%\rt\bin\D3D12Core.dll" if exist "%RTGL_BIN%\rt" rmdir "%RTGL_BIN%\rt" 2>nul
if not exist "%RTGL_BIN%\rt\bin\D3D12Core.dll" mklink /J "%RTGL_BIN%\rt" "%RTGL_DIR%" >nul
REM CWD = exe dir so base.dll / d3xp.dll resolve there.
pushd "%RTGL_BIN%"
"%RTGL_EXE%" +set r_rtgl_enable 1 +set r_rtgl_overridePath "%RTGL_URI%" +set fs_basepath "%GAMEDATA%" +set r_fullscreen 0 +set win_viewlog 1 +set rt_dbg_dumpLights 60 %*
set "RC=%ERRORLEVEL%"
popd
if "%RC%"=="0" goto :done
echo [dhewm3 exited with code %RC%]
goto :fail

:done
endlocal
exit /b 0

:fail
if defined DOPAUSE pause
endlocal
exit /b 1
