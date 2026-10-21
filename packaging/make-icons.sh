#!/bin/sh
# Regenerate pc-app/gui/phonemic.ico from the icon master, packaging/linux/phonemic.svg.
# Needs rsvg-convert and python3. Sizes up to 24 px drop the sound arcs and
# thicken the strokes, which would otherwise blur into the background.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
svg=$root/packaging/linux/phonemic.svg
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -e '/opacity="0.55"/d' -e 's/stroke-width="14"/stroke-width="22"/' "$svg" > "$tmp/small.svg"
for n in 16 20 24 32 40 48 64 128 256; do
    src=$svg
    [ "$n" -le 24 ] && src=$tmp/small.svg
    rsvg-convert -w "$n" -h "$n" "$src" -o "$tmp/$n.png"
done
# An .ico whose entries are the PNGs themselves (Windows Vista+ reads those),
# so the file stays small.
python3 - "$tmp" "$root/pc-app/gui/phonemic.ico" <<'PY'
import struct, sys
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
pngs = [open(f"{sys.argv[1]}/{n}.png", "rb").read() for n in sizes]
out = struct.pack("<HHH", 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, png in zip(sizes, pngs):
    out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(png), off)
    off += len(png)
open(sys.argv[2], "wb").write(out + b"".join(pngs))
PY
echo "wrote pc-app/gui/phonemic.ico"
