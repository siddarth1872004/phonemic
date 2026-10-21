#!/bin/sh
# Builds PhoneMic-x86_64.AppImage from an installed tree (cmake --install).
#
#   packaging/linux/build-appimage.sh <install-prefix> <appimagetool> [output]
#
# The binaries load X11/Wayland/GL and the sound server (libpulse) from the
# system at run time, so nothing else has to be bundled.
set -eu
prefix=$1
tool=$2
out=${3:-PhoneMic-x86_64.AppImage}
here=$(cd "$(dirname "$0")" && pwd)

appdir=$(mktemp -d)/PhoneMic.AppDir
mkdir -p "$appdir/usr"
cp -R "$prefix/bin" "$prefix/share" "$appdir/usr/"
cp "$here/phonemic.desktop" "$appdir/phonemic.desktop"
cp "$here/phonemic.svg" "$appdir/phonemic.svg"
ln -s phonemic.svg "$appdir/.DirIcon"
cat > "$appdir/AppRun" <<'RUN'
#!/bin/sh
# PhoneMic.AppImage: the app by default; "PhoneMic.AppImage receiver ..." etc.
# run the command-line tools.
here=$(dirname "$(readlink -f "$0")")
case "${1:-}" in
    receiver|vmic-check) tool=$1; shift; exec "$here/usr/bin/phonemic-$tool" "$@" ;;
esac
exec "$here/usr/bin/phonemic" "$@"
RUN
chmod +x "$appdir/AppRun"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$appdir" "$out"
echo "built $out"
