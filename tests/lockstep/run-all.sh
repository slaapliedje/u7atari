#!/bin/sh
# Runs every scenario here through tools/lockstep-compare.sh, on each machine named, all at once,
# and prints one line per run.
#   tests/lockstep/run-all.sh <bg-static> <si-static> <out-dir> [tt] [falcon]
# <bg-static> and <si-static> are the games' STATIC folders (only read). The start states, named
# by each script's "#@start" line, are made in <out-dir>/start: bg-new and si-new are STATIC with
# an empty GAMEDAT, and bg-start is what u7host leaves after bg-newgame.scr (it ends by saving).
# A script's "#@args" line holds its game arguments; \0377 and the like are bytes.
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
work=$out/make-bg-start
rm -rf "${work:?}"
cp -r "$start/bg-new" "$work"
cp "$here/tests/lockstep/bg-newgame.scr" "$work/TEST.SCR"
"$here/build-host/u7host" --data "$work" --lockstep --script TEST.SCR > "$out/make-bg-start.log" 2>&1
mkdir -p "$start/bg-start"
cp -r "$work/STATIC" "$work/GAMEDAT" "$work/GAME00.U7" "$start/bg-start/"

for script in "$here"/tests/lockstep/*.scr; do
	name=$(basename "$script" .scr)
	state=$(sed -n 's/^#@start //p' "$script")
	args=$(sed -n 's/^#@args *//p' "$script")
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
