@echo off
REM ============================================================
REM  Build AdjustableSpellCost (Release).
REM  Configures CMake automatically on first run, then builds and
REM  copies the DLL (+ default .ini) into release\.
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

if not exist "%PROJ%\release\" mkdir "%PROJ%\release"
copy /Y "%PROJ%\build\Release\AdjustableSpellCost.dll" "%PROJ%\release\" >nul
if errorlevel 1 goto :fail
copy /Y "%PROJ%\AdjustableSpellCost.ini" "%PROJ%\release\" >nul

echo.
echo [build] OK -^> "%PROJ%\release\AdjustableSpellCost.dll"
goto :end

:fail
echo.
echo [build] BUILD FAILED.
endlocal
exit /b 1

:end
endlocal
pause
