#!/bin/sh
# The engine's music (ENGINE/MUSIC.FLX): the original's AdLib songs played by its own song player
# and music driver (u7musdro), their register writes recorded at the 60 Hz tick and packed.
#
#   mkmusic.sh <u7musdro> <STATIC dir> <out MUSIC.FLX>
#
# u7musdro is a target of the PC build (cmake --build build-host --target u7musdro).
set -e
[ $# -eq 3 ] || { echo "usage: mkmusic.sh <u7musdro> <STATIC dir> <out MUSIC.FLX>" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$1" "$2" "$work" > /dev/null
python3 "$(dirname "$0")/muspack.py" "$work/index.txt" "$work" "$3"
