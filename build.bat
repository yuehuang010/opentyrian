@echo off
rem ---------------------------------------------------------------------------
rem Build and run OpenTyrian on Windows, from the repository root.
rem
rem   build.bat                  configure (if needed), build, then run
rem   build.bat --build-only     build only, do not run
rem   build.bat --run-only       run the existing executable, do not build
rem   build.bat --debug          use the Debug configuration (asserts on)
rem   build.bat --clean          delete the build directory, then build and run
rem   build.bat -- <args>        forward <args> to opentyrian.exe
rem
rem The game is launched with the repository root as its working directory, so
rem it picks up the tyrian.base / tyrian.hd bundles that live here.
rem
rem SDL2 and SDL2_net are located via CMAKE_PREFIX_PATH. Override the default
rem search paths by setting OPENTYRIAN_SDL_PREFIX before running this script.
rem ---------------------------------------------------------------------------

setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "BUILD_TYPE=Release"
set "DO_BUILD=1"
set "DO_RUN=1"
set "DO_CLEAN=0"
set "GAME_ARGS="

:parse_args
if "%~1"=="" goto parse_done
if /i "%~1"=="--build-only" ( set "DO_RUN=0" & shift & goto parse_args )
if /i "%~1"=="--run-only"   ( set "DO_BUILD=0" & shift & goto parse_args )
if /i "%~1"=="--debug"      ( set "BUILD_TYPE=Debug" & shift & goto parse_args )
if /i "%~1"=="--clean"      ( set "DO_CLEAN=1" & shift & goto parse_args )
if /i "%~1"=="--help"       goto usage
if /i "%~1"=="-h"           goto usage
if "%~1"=="--" (
    shift
    goto collect_game_args
)
echo [build] Unknown option: %~1
goto usage

:collect_game_args
if "%~1"=="" goto parse_done
set "GAME_ARGS=%GAME_ARGS% %1"
shift
goto collect_game_args

:parse_done

if /i "%BUILD_TYPE%"=="Debug" (
    set "BUILD_DIR=%ROOT%\build-ninja-debug"
) else (
    set "BUILD_DIR=%ROOT%\build-ninja"
)

set "EXE=%BUILD_DIR%\opentyrian.exe"

if "%DO_BUILD%"=="0" goto run

rem --- Locate Visual Studio (for cl.exe, and its bundled cmake/ninja) ---------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [build] ERROR: no Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [build] ERROR: vcvars64.bat not found under "%VSPATH%".
    exit /b 1
)

rem --- Prefer cmake/ninja on PATH, else the copies bundled with Visual Studio -
set "VS_CMAKE_BIN=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"

set "CMAKE="
for /f "delims=" %%i in ('where cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%i"
if not defined CMAKE if exist "%VS_CMAKE_BIN%\CMake\bin\cmake.exe" set "CMAKE=%VS_CMAKE_BIN%\CMake\bin\cmake.exe"
if not defined CMAKE (
    echo [build] ERROR: cmake.exe not found on PATH or under "%VS_CMAKE_BIN%".
    exit /b 1
)

set "NINJA="
for /f "delims=" %%i in ('where ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%i"
if not defined NINJA if exist "%VS_CMAKE_BIN%\Ninja\ninja.exe" set "NINJA=%VS_CMAKE_BIN%\Ninja\ninja.exe"
if not defined NINJA (
    echo [build] ERROR: ninja.exe not found on PATH or under "%VS_CMAKE_BIN%".
    exit /b 1
)

if "%DO_CLEAN%"=="1" (
    if exist "%BUILD_DIR%" (
        echo [build] Removing "%BUILD_DIR%"
        rmdir /s /q "%BUILD_DIR%"
    )
)

rem --- Enter the MSVC x64 environment (cl.exe, headers, libs) -----------------
rem vcvars64.bat itself shells out to a bare `vswhere.exe`, so make sure the
rem VS Installer directory is on PATH or it prints a spurious "not recognized".
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: vcvars64.bat failed.
    exit /b 1
)

rem --- Configure on first use --------------------------------------------------
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    if not defined OPENTYRIAN_SDL_PREFIX set "OPENTYRIAN_SDL_PREFIX=C:/source/deps/SDL2-2.30.9;C:/source/deps/SDL2_net-2.2.0"
    echo [build] Configuring %BUILD_TYPE% build in "%BUILD_DIR%"
    "%CMAKE%" -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        -DCMAKE_PREFIX_PATH="%OPENTYRIAN_SDL_PREFIX%"
    if errorlevel 1 (
        echo [build] ERROR: CMake configuration failed.
        exit /b 1
    )
)

echo [build] Building %BUILD_TYPE%
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo [build] ERROR: build failed.
    exit /b 1
)

:run
if "%DO_RUN%"=="0" (
    echo [build] Done: "%EXE%"
    exit /b 0
)

if not exist "%EXE%" (
    echo [build] ERROR: "%EXE%" not found. Run without --run-only to build it first.
    exit /b 1
)

echo [build] Running "%EXE%"%GAME_ARGS%
pushd "%ROOT%"
"%EXE%"%GAME_ARGS%
set "GAME_EXIT=%ERRORLEVEL%"
popd
exit /b %GAME_EXIT%

:usage
echo Usage: build.bat [--build-only^|--run-only] [--debug] [--clean] [-- ^<game args^>]
exit /b 1
