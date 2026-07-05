@echo off
rem ===== PhoneMic — double-click to run =====
cd /d "%~dp0"

if not exist "target\release\phonemic-receiver.exe" (
    echo First run: building PhoneMic ^(one time, ~1 min^)...
    cargo build --release -p phonemic-core
    if errorlevel 1 goto builderror
)

target\release\phonemic-receiver.exe
pause
goto :eof

:builderror
echo.
echo Build failed. Make sure Rust is installed ^(https://rustup.rs^).
pause
