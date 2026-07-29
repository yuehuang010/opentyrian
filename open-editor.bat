@echo off
rem ---------------------------------------------------------------------------
rem open-editor.bat -- double-click in Explorer to launch the OpenTyrian level
rem editor. The Windows counterpart of open-editor.command (macOS).
rem
rem Unlike the .command script this one does NOT build: it expects the game to
rem have been built already (use build.bat). It cd's to its own directory so
rem the relative binary/data paths resolve no matter what working directory
rem Explorer hands it, then boots straight into the editor, which shows its own
rem in-app episode picker (1-4) followed by that episode's level-select screen.
rem See internal/plan/LEVEL_EDITOR_PLAN.md for the editor.
rem ---------------------------------------------------------------------------

setlocal EnableExtensions

rem Resolve this script's own directory (the repo root) and work from there.
cd /d "%~dp0" || exit /b 1

set "DATA_DIR=.\tyrian21"

rem Prefer the Release build, fall back to the Debug one (see build.bat).
set "BINARY=.\build-ninja\opentyrian.exe"
if not exist "%BINARY%" set "BINARY=.\build-ninja-debug\opentyrian.exe"

echo === OpenTyrian level editor ===
echo.

if not exist "%BINARY%" (
    echo Binary not found in build-ninja\ or build-ninja-debug\.
    echo Build it first:  build.bat --build-only
    echo.
    pause
    exit /b 1
)

rem The game ships with no data; the editor needs the Tyrian 2.1 files.
if not exist "%DATA_DIR%\" (
    echo Data directory '%DATA_DIR%' not found.
    echo Place the Tyrian 2.1 data files there ^(https://camanis.net/tyrian/tyrian21.zip^).
    echo.
    pause
    exit /b 1
)

echo Launching editor...
echo (You'll pick an episode, then a level, inside the editor itself.)
echo (In-editor: T tile sidebar, [ ] pick tile, Enter place, U/R undo/redo,
echo  E events, S save, X export, Esc back. F1 toggles the help lines.)
echo (Mouse: click/drag selects the cell, right-click picks; click the left strip to scroll, wheel scrolls. Place with Enter/Space.)
echo.

"%BINARY%" --data "%DATA_DIR%" --edit
set "GAME_EXIT=%ERRORLEVEL%"

if not "%GAME_EXIT%"=="0" (
    echo.
    echo Editor exited with code %GAME_EXIT%.
    pause
)

exit /b %GAME_EXIT%
