@echo off
rem ===== PhoneMic — double-click to run the windowed app =====
cd /d "%~dp0"

if not exist "target\release\PhoneMic.exe" (
    echo First run: building PhoneMic ^(one time, ~4 min^)...
    if exist "%USERPROFILE%\w64devkit\bin" set "PATH=%USERPROFILE%\w64devkit\bin;%PATH%"
    cargo build --release -p phonemic-gui
    if errorlevel 1 goto builderror
)

start "" "target\release\PhoneMic.exe"
goto :eof

:builderror
echo.
echo Build failed. Make sure Rust is installed ^(https://rustup.rs^).
pause
