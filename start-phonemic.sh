#!/usr/bin/env sh
# PhoneMic — run the desktop app on Linux/macOS, building it the first time.
set -e
cd "$(dirname "$0")"
BIN=build/bin/PhoneMic
if [ ! -x "$BIN" ]; then
    echo "First run: building PhoneMic (one time, a few minutes)..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
fi
exec "$BIN" "$@"
