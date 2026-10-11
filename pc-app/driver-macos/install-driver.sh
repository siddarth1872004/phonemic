#!/bin/sh
# Install (or remove) the PhoneMic virtual microphone for all users of this Mac.
#
#   ./install-driver.sh [path/to/PhoneMic.driver]    install / update
#   ./install-driver.sh --uninstall                  remove
#
# PhoneMic.driver is a Core Audio plug-in (no kernel extension): it is copied to
# /Library/Audio/Plug-Ins/HAL and Core Audio is restarted to load it. Needs an
# administrator password (the PhoneMic app asks for it through macOS).
set -eu

dst=/Library/Audio/Plug-Ins/HAL/PhoneMic.driver
here=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
    exec sudo /bin/sh "$here/$(basename "$0")" "$@"
fi

restart_core_audio() {
    # launchd starts coreaudiod again right away (kickstart is blocked by SIP).
    killall coreaudiod 2>/dev/null || true
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$dst"
    restart_core_audio
    echo "PhoneMic Microphone removed."
    exit 0
fi

src=${1:-$here/PhoneMic.driver}
if [ ! -f "$src/Contents/Info.plist" ]; then
    echo "PhoneMic.driver not found at: $src" >&2
    exit 1
fi

mkdir -p /Library/Audio/Plug-Ins/HAL
rm -rf "$dst"
cp -R "$src" "$dst"
chown -R root:wheel "$dst"
chmod -R go-w "$dst"
# A downloaded copy carries the quarantine flag, which stops Core Audio loading it.
xattr -dr com.apple.quarantine "$dst" 2>/dev/null || true
restart_core_audio
echo "PhoneMic Microphone installed. Pick \"PhoneMic Microphone\" as the microphone in your apps."
