#!/bin/sh
# Runs every scenario here through tools/lockstep-compare.sh, on each machine named, all at once,
# and prints one line per run.
#   tests/lockstep/run-all.sh <bg-static> <si-static> <out-dir> [tt] [falcon]
# <bg-static> and <si-static> are the games' STATIC folders (only read). The start states, named
# by each script's "#@start" line, are made in <out-dir>/start: bg-new and si-new are STATIC with
# an empty GAMEDAT, and bg-start and si-start are what u7host leaves after bg-newgame.scr and
# si-newgame.scr (they end by saving).
# A script's "#@args" line holds its game arguments; \0377 and the like are bytes.
# bg-import.scr runs only when EXULT_SAVE names an Exult saved game of the Black Gate, si-import.scr
# only when EXULT_SAVE_SI names one of Serpent Isle.
# Needs build-host, build-host-si, build-dbg and build-si-dbg built from the same sources.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
bg=$1 si=$2 out=$3
shift 3
machines=${*:-tt falcon}
start=$out/start

rm -rf "${start:?}"
for s in bg-new si-new; do
	mkdir -p "$start/$s/GAMEDAT"
done
cp -r "$bg" "$start/bg-new/STATIC"
cp -r "$si" "$start/si-new/STATIC"
# bg-start and si-start: what bg-newgame.scr and si-newgame.scr leave (both end by saving).
for g in bg si; do
	case $g in bg) host=$here/build-host/u7host ;; *) host=$here/build-host-si/u7host ;; esac
	s=$g-start work=$out/make-$g-start
	rm -rf "${work:?}"
	cp -r "$start/$g-new" "$work"
	cp "$here/tests/lockstep/$g-newgame.scr" "$work/TEST.SCR"
	"$host" --data "$work" --lockstep --script TEST.SCR > "$out/make-$s.log" 2>&1
	mkdir -p "$start/$s"
	cp -r "$work/STATIC" "$work/GAMEDAT" "$work/GAME00.U7" "$start/$s/"
done
# bg-import and si-import: STATIC and, as GAMEDAT, the Exult saved game EXULT_SAVE (the Black
# Gate) or EXULT_SAVE_SI (Serpent Isle) names, converted by tools/exult2u7.py; without it the
# scenarios starting from it are left out.
if [ -n "${EXULT_SAVE:-}" ]; then
	mkdir -p "$start/bg-import/GAMEDAT"
	cp -r "$bg" "$start/bg-import/STATIC"
	python3 "$here/tools/exult2u7.py" "$EXULT_SAVE" "$bg" "$start/bg-import/GAMEDAT" > /dev/null 2>&1
fi
if [ -n "${EXULT_SAVE_SI:-}" ]; then
	mkdir -p "$start/si-import/GAMEDAT"
	cp -r "$si" "$start/si-import/STATIC"
	python3 "$here/tools/exult2u7.py" "$EXULT_SAVE_SI" "$si" "$start/si-import/GAMEDAT" > /dev/null 2>&1
fi

for script in "$here"/tests/lockstep/*.scr; do
	name=$(basename "$script" .scr)
	state=$(sed -n 's/^#@start //p' "$script")
	args=$(sed -n 's/^#@args *//p' "$script")
	[ -d "$start/$state" ] || continue
	case $name in si-*) si_run=1 ;; *) si_run=0 ;; esac
	for m in $machines; do
		(
			# shellcheck disable=SC2086 # the arguments are words
			set -- $(printf '%b' "$args")
			U7_SI=$si_run "$here/tools/lockstep-compare.sh" "$m" "$start/$state" "$script" \
				"$out/$name-$m" "$@" > "$out/$name-$m.txt" 2>&1 && r=pass || r=FAIL
			echo "$r $name $m: $(head -1 "$out/$name-$m.txt")"
		) &
	done
done
wait
