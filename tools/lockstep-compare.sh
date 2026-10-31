#!/bin/sh
# Plays one script in lockstep on the PC reference (u7host) and on the Atari debug build under
# Hatari, then compares them: every "LS" checksum line, and every file the game left in its
# folder (saves, screenshots). Exits 0 when they match.
#   tools/lockstep-compare.sh <tt|falcon> <game-dir> <script> <out-dir> [game arguments...]
# <game-dir> holds STATIC, and GAMEDAT and saved games to start from; it is copied, never
# written. The game arguments go to both, after "--lockstep --script TEST.SCR"; for the main
# game alone use: --program u7 p (Serpent Isle: --program si p).
# U7_SI=1 compares Serpent Isle (build-host-si/u7host and build-si-dbg/SERPENT.PRG).
# U7_VBLS caps the Atari run (default 3000000); it ends sooner when the script quits.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
machine=$1 game=$2 script=$3 out=$4
shift 4

if [ "${U7_SI:-0}" = 1 ]; then
	host=$here/build-host-si/u7host prg=$here/build-si-dbg/SERPENT.PRG arg=SERPENT.ARG
else
	host=$here/build-host/u7host prg=$here/build-dbg/ULTIMA7.PRG arg=ULTIMA7.ARG
fi
rm -rf "${out:?}/pc" "${out:?}/atari"
mkdir -p "$out/pc" "$out/atari/U7"
for d in "$out/pc" "$out/atari/U7"; do
	cp -r "$game"/. "$d"/
	cp "$script" "$d/TEST.SCR"
done

"$host" --data "$out/pc" --lockstep --script TEST.SCR "$@" > "$out/pc.log" 2>&1 || true
echo "--lockstep --natfeats --script TEST.SCR $*" > "$out/atari/U7/$arg"
U7_PRG=$prg "$here/tools/hatari-test.sh" "$machine" "$out/atari" "${U7_VBLS:-3000000}" --natfeats on \
	> "$out/atari.log" 2>&1 || true
rm -f "$out/atari/U7/$(basename "$prg")" "$out/atari/U7/$arg"

tr -d '\r' < "$out/pc.log" | grep -a '^LS ' > "$out/pc.ls" || true
tr -d '\r' < "$out/atari.log" | grep -a '^LS ' > "$out/atari.ls" || true
status=0
if cmp -s "$out/pc.ls" "$out/atari.ls"; then
	echo "checksums: $(wc -l < "$out/pc.ls") lines match ($(tail -1 "$out/pc.ls"))"
else
	status=1
	first=$(diff "$out/pc.ls" "$out/atari.ls" | head -1)
	echo "checksums differ ($first): $(wc -l < "$out/pc.ls") lines on the PC, $(wc -l < "$out/atari.ls") on the Atari"
	diff "$out/pc.ls" "$out/atari.ls" | head -6
fi
if diff -r -q "$out/pc" "$out/atari/U7" > "$out/files.diff"; then
	echo "files: the same"
else
	status=1
	echo "files differ:"
	cat "$out/files.diff"
fi
exit $status
