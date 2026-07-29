@echo off
REM ============================================================
REM  Build LanternTorch (Release).
REM  Configures CMake automatically on first run, then builds.
REM  Output: build\Release\LanternTorch.dll
REM ============================================================
setlocal
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

where cmake >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: cmake not found in PATH.
    goto :fail
)

if not exist "%PROJ%\build\" (
    echo [build] First run - configuring CMake project...
    cmake -S "%PROJ%" -B "%PROJ%\build" -A x64
    if errorlevel 1 goto :fail
)

echo [build] Building Release...
cmake --build "%PROJ%\build" --config Release
if errorlevel 1 goto :fail

echo.
echo [build] OK -^> "%PROJ%\build\Release\LanternTorch.dll"
goto :end

:fail
echo.
echo [build] BUILD FAILED.
endlocal
exit /b 1

:end
endlocal
pause
