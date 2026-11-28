#!/bin/sh
# The engine's music (ENGINE/MUSIC.FLX): the original's AdLib songs played by its own song player
# and music driver (u7musdro), their register writes recorded at the 60 Hz tick and packed; the
# intro's and main menu's songs (INTROADM.DAT) with them as songs 64 on. With adlconvert and a
# second output, also the intro's sounds (ENGINE/INTRO.FLX): its eight synth effects rendered,
# and the Guardian's speech.
#
#   mkmusic.sh <u7musdro> <STATIC dir> <out MUSIC.FLX> [<adlconvert> <out INTRO.FLX>]
#
# u7musdro is a target of the PC build (cmake --build build-host --target u7musdro).
set -e
[ $# -eq 3 ] || [ $# -eq 5 ] || {
	echo "usage: mkmusic.sh <u7musdro> <STATIC dir> <out MUSIC.FLX> [<adlconvert> <out INTRO.FLX>]" >&2
	exit 2
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$1" "$2" "$work" > /dev/null
python3 "$(dirname "$0")/muspack.py" "$work/index.txt" "$work" "$3"
if [ $# -eq 5 ]; then
	for f in "$work"/isfx*.dro; do
		"$4" render "$f" -o "${f%.dro}.wav" --rate 15980 --tail 200 > /dev/null
	done
	python3 "$(dirname "$0")/intropack.py" "$work/isfx.txt" "$work" "$2" "$5"
fi
