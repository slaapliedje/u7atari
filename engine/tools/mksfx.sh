#!/bin/sh
# The engine's sounds (ENGINE/SFX.FLX): the original's AdLib effects played by its own driver
# (u7sfxdro), rendered by adlconvert (the AdLib converter, with Nuked OPL3) and packed.
#
#   mksfx.sh <u7sfxdro> <adlconvert> <STATIC dir> <out SFX.FLX>
#
# u7sfxdro is a target of the PC build (cmake --build build-host --target u7sfxdro).
set -e
[ $# -eq 4 ] || { echo "usage: mksfx.sh <u7sfxdro> <adlconvert> <STATIC dir> <out SFX.FLX>" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$1" "$3" "$work"
for f in "$work"/sfx*.dro; do
	"$2" render "$f" -o "${f%.dro}.wav" --rate 15980 --tail 200 > /dev/null
done
python3 "$(dirname "$0")/sfxpack.py" "$work/index.txt" "$work" "$4"
