@echo off
rem ===== PhoneMic — double-click to run the desktop app =====
rem Uses a downloaded PhoneMic.exe next to this file if present; otherwise
rem builds it once from source (needs CMake + Visual Studio Build Tools).
cd /d "%~dp0"

if exist "PhoneMic.exe" (
    start "" "PhoneMic.exe"
    goto :eof
)
if exist "build\bin\Release\PhoneMic.exe" goto run

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found.
    echo Easiest: download PhoneMic.exe from the Releases page and put it next to this file.
    echo To build from source install "Visual Studio Build Tools" ^(C++ workload^) and CMake.
    pause
    goto :eof
)
echo First run: building PhoneMic ^(one time, a few minutes^)...
cmake -S . -B build -A x64 || goto builderror
cmake --build build --config Release --parallel || goto builderror

:run
start "" "build\bin\Release\PhoneMic.exe"
goto :eof

:builderror
echo.
echo Build failed. See docs\BUILDING.md.
pause
