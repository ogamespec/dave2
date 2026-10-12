@echo off
rem ---------------------------------------------------------------
rem  Builds the SDL3 port with MSVC (Visual Studio 2022/2026).
rem  Run from a "Developer Command Prompt", or let this script find
rem  VS via vswhere / the standard install layout.
rem ---------------------------------------------------------------
setlocal

set HERE=%~dp0
set PROJ=%HERE%dd2.sln
set CFG=%1
if "%CFG%"=="" set CFG=Release
rem Second argument selects the architecture: x64 (default) or x86.
set ARCH=%2
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="x86"   set PLAT=Win32
if /i "%ARCH%"=="win32" set PLAT=Win32
if /i "%ARCH%"=="x64"   set PLAT=x64
if not defined PLAT (
    echo Unknown architecture "%ARCH%". Use x64 or x86.
    exit /b 1
)

rem --- make sure SDL3 is present -----------------------------------
if not exist "%HERE%..\third_party\SDL3\include\SDL3\SDL.h" (
    echo SDL3 not found, fetching it...
    powershell -ExecutionPolicy Bypass -File "%HERE%fetch_sdl3.ps1" || goto :err
)

rem --- locate msbuild if we are not in a developer prompt ----------
rem NOTE: keep these out of parenthesised blocks -- "%ProgramFiles(x86)%"
rem contains a ')' and breaks cmd's block parsing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS2026=%ProgramFiles%\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set "MSBUILD="

where msbuild >nul 2>nul && set "MSBUILD=msbuild"
if not defined MSBUILD if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD if exist "%VS2026%" set "MSBUILD=%VS2026%"
if not defined MSBUILD for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\*") do if not defined MSBUILD for /d %%e in ("%%d\*") do if exist "%%e\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%%e\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD echo Could not find MSBuild. Open a Developer Command Prompt and re-run. & goto :err

echo Building %CFG% %PLAT% with:
echo   %MSBUILD%
"%MSBUILD%" "%PROJ%" /p:Configuration=%CFG% /p:Platform=%PLAT% /m /v:minimal
if errorlevel 1 goto :err
echo.
if /i "%PLAT%"=="Win32" (
    echo Done: %HERE%..\bin\%CFG%\x86\dd2.exe
) else (
    echo Done: %HERE%..\bin\%CFG%\dd2.exe
)
echo Copy the game files (DD2.EXE, *.DD2) next to the executable, or pass the
echo game directory as the first argument:  dd2.exe C:\Games\DD2
exit /b 0

:err
echo BUILD FAILED
exit /b 1
