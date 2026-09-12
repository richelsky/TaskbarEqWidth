@echo off
rem ===========================================================================
rem  TaskbarEqWidth launcher
rem
rem  Purpose: the manager (TaskbarEqWidth.exe) is a CONSOLE program. If you
rem  double-click the exe, a console window opens and vanishes the moment the
rem  command finishes -- so it looks like "nothing happened" even when it did
rem  work. This launcher keeps the window open and gives you a menu.
rem
rem  Text in this file is intentionally ASCII-only. Windows cmd.exe parses
rem  batch files using the console code page; multi-byte characters can eat
rem  the following "&" or ")" and corrupt the command. The exe prints its own
rem  Chinese output and sets the console code page itself, so the user-visible
rem  messages are still localized.
rem ===========================================================================
setlocal
title TaskbarEqWidth
cd /d "%~dp0"

rem ---- injecting into explorer.exe requires administrator rights ----
net session >nul 2>&1
if errorlevel 1 goto :elevate

if not exist "TaskbarEqWidth.exe" goto :nodll
if not exist "TaskbarEqWidthHook.dll" goto :nodll

:menu
cls
echo ================================================================
echo    TaskbarEqWidth - same-width taskbar buttons (Win11)
echo ================================================================
echo.
echo    [1] Install now                 (default width 176)
echo    [2] Install with a custom width
echo    [3] Show status                 (is the hook really active?)
echo    [4] Scan for leftovers          (read-only, changes nothing)
echo    [5] Uninstall completely
echo    [0] Exit
echo.
set "CH="
set /p "CH=Choose 0-5: "
if "%CH%"=="1" goto :install
if "%CH%"=="2" goto :install_custom
if "%CH%"=="3" goto :status
if "%CH%"=="4" goto :verify
if "%CH%"=="5" goto :uninstall
if "%CH%"=="0" exit /b 0
goto :menu

:install
call :run
goto :menu

:install_custom
set "W="
set /p "W=Width in DIP, 50-400 (try 140-220): "
if "%W%"=="" goto :menu
call :run --width %W%
goto :menu

:status
call :run --status
goto :menu

:verify
call :run --verify
goto :menu

:uninstall
call :run --uninstall
goto :menu

rem ---- shared runner: show output, then wait for a key press ---------------
:run
echo.
echo ----------------------------------------------------------------
TaskbarEqWidth.exe %*
echo ----------------------------------------------------------------
echo.
echo (press any key to return to the menu)
pause >nul
exit /b 0

:elevate
echo.
echo Requesting administrator rights (needed to inject into explorer)...
echo.
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
exit /b 0

:nodll
echo.
echo [x] TaskbarEqWidthHook.dll is missing from this folder.
echo     TaskbarEqWidth.exe and TaskbarEqWidthHook.dll must stay together.
echo.
pause
exit /b 1
