@echo off
REM ============================================================
REM  Build CustomTalismanEffects (Release).
REM  Bumps release\version.md, configures CMake, then builds.
REM  Copies DLL to release folder after successful build.
REM  Output: build\Release\CustomTalismanEffects.dll
REM         release\CustomTalismanEffects.dll
REM ============================================================
setlocal enabledelayedexpansion
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

where cmake >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: cmake not found in PATH.
    goto :fail
)

REM ---- version -------------------------------------------------------------
REM release\version.md is the single source of truth and always names the
REM version of the DLL currently sitting in release\. CMake reads it to generate
REM the VERSIONINFO resource (see src\CustomTalismanEffects.rc.in -- an
REM identity-less binary is a direct contributor to the ML/heuristic AV false
REM positives), so the bump has to happen BEFORE configuring.
REM
REM The stamp file records "release\version.md has already been shipped", which
REM is what distinguishes the very first build (use 1.4.0 as written) from every
REM later one (bump the patch first).
set "VERFILE=%PROJ%\release\version.md"
set "VERSTAMP=%PROJ%\release\.version_built"
if not exist "%PROJ%\release\" mkdir "%PROJ%\release\"
if not exist "%VERFILE%" (
    echo [build] Creating version.md starting at 1.4.0...
    echo 1.4.0> "%VERFILE%"
    set "NEWVER=1.4.0"
) else (
    set "CURVER="
    for /f "usebackq delims=" %%v in ("%VERFILE%") do if not defined CURVER set "CURVER=%%v"
    if not exist "%VERSTAMP%" (
        REM First build of a hand-written version: ship it as written.
        set "NEWVER=!CURVER!"
    ) else (
        for /f "tokens=1,2,3 delims=." %%a in ("!CURVER!") do (
            set "VMAJOR=%%a"
            set "VMINOR=%%b"
            set "VPATCH=%%c"
        )
        set /a VPATCH+=1
        set "NEWVER=!VMAJOR!.!VMINOR!.!VPATCH!"
        echo !NEWVER!> "%VERFILE%"
        REM Bumping before the build means a FAILED build would otherwise burn a
        REM version number. :fail restores this.
        set "PREVVER=!CURVER!"
    )
)
echo [build] Version -^> !NEWVER!

REM Configure every run: configure_file() is what stamps the new version into
REM the resource, and it only runs at configure time. Re-configuring an existing
REM tree is cheap.
echo [build] Configuring CMake project...
cmake -S "%PROJ%" -B "%PROJ%\build" -A x64
if errorlevel 1 goto :fail

echo [build] Building Release...
cmake --build "%PROJ%\build" --config Release
if errorlevel 1 goto :fail

REM Create release folder if it doesn't exist
if not exist "%PROJ%\release\" (
    echo [build] Creating release folder...
    mkdir "%PROJ%\release\"
)

REM Stage then replace rather than copying over the existing file in place.
REM This keeps release\CustomTalismanEffects.dll independent from the build
REM output even if a previous tool made them hard links; loading the release DLL
REM must never lock the build output and prevent the next link step.
echo [build] Copying DLL to release folder...
set "STAGED_DLL=%PROJ%\release\CustomTalismanEffects.dll.new"
copy /Y /B "%PROJ%\build\Release\CustomTalismanEffects.dll" "%STAGED_DLL%" >nul
if errorlevel 1 (
    echo [build] ERROR: Failed to stage DLL for the release folder.
    goto :fail
)
move /Y "%STAGED_DLL%" "%PROJ%\release\CustomTalismanEffects.dll" >nul
if errorlevel 1 (
    echo [build] ERROR: Failed to replace release\CustomTalismanEffects.dll.
    goto :fail
)

REM Record that this version has shipped, so the next run bumps.
echo !NEWVER!> "%VERSTAMP%"

echo.
echo [build] OK -^> "%PROJ%\build\Release\CustomTalismanEffects.dll"  (v!NEWVER!)
echo [build] OK -^> "%PROJ%\release\CustomTalismanEffects.dll"
goto :end

:fail
REM Give back the version number the pre-configure bump consumed, so a failed
REM build doesn't leave a gap in the release history.
if defined PREVVER (
    echo !PREVVER!> "%VERFILE%"
    echo [build] Version rolled back -^> !PREVVER!
)
echo.
echo [build] BUILD FAILED.
endlocal
exit /b 1

:end
endlocal
REM Keep double-click behavior friendly, but let CI/verification callers finish.
if /I not "%~1"=="--no-pause" pause
