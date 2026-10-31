#!/bin/sh
# Runs every scenario on u7host built with AddressSanitizer and UndefinedBehaviorSanitizer, and
# lists each distinct report once with where it happened: memory faults the DOS game got away
# with, which can make the Atari build behave differently, or crash.
#   tests/lockstep/sanitize-all.sh <bg-static> <si-static> <out-dir>
# Then each runs under valgrind's memcheck, for values used before they are set (u7host built
# with -DCMAKE_BUILD_TYPE=RelWithDebInfo in build-host-vg and build-host-si-vg).
# Needs build-host-asan and build-host-si-asan:
#   F="-m32 -fsanitize=address,undefined -fno-sanitize=alignment -fsanitize-recover=address"
#   cmake -B build-host-asan -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" \
#       -DCMAKE_EXE_LINKER_FLAGS="-m32 -fsanitize=address,undefined"
# (and build-host-si-asan with -DU7_GAME="Serpent Isle"). Unaligned accesses are left out: the
# 68030 and x86 both make them. So are new[] freed by delete and deletes through a base class,
# which Borland's heap and ours treat alike.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
bg=$1 si=$2 out=$3
start=$out/start
export ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0:new_delete_type_mismatch=0:halt_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1

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
"$here/build-host/u7host" --data "$work" --lockstep --script TEST.SCR > /dev/null 2>&1
mkdir -p "$start/bg-start"
cp -r "$work/STATIC" "$work/GAMEDAT" "$work/GAME00.U7" "$start/bg-start/"

for script in "$here"/tests/lockstep/*.scr; do
	name=$(basename "$script" .scr)
	state=$(sed -n 's/^#@start //p' "$script")
	args=$(sed -n 's/^#@args *//p' "$script")
	case $name in si-*) host=$here/build-host-si-asan/u7host ;; *) host=$here/build-host-asan/u7host ;; esac
	dir=$out/$name
	rm -rf "${dir:?}"
	cp -r "$start/$state" "$dir"
	cp "$script" "$dir/TEST.SCR"
	# shellcheck disable=SC2086 # the arguments are words
	set -- $(printf '%b' "$args")
	"$host" --data "$dir" --lockstep --script TEST.SCR "$@" > "$out/$name.log" 2>&1 || true
done

# The stack: the Atari build has 256 KB (_stksize in platform/atari/main.c); each scenario must
# run on the PC in half that, or the Atari's could overflow into MiNTLib's variables.
for script in "$here"/tests/lockstep/*.scr; do
	name=$(basename "$script" .scr)
	state=$(sed -n 's/^#@start //p' "$script")
	args=$(sed -n 's/^#@args *//p' "$script")
	case $name in si-*) host=$here/build-host-si/u7host ;; *) host=$here/build-host/u7host ;; esac
	dir=$out/stack-$name
	rm -rf "${dir:?}"
	cp -r "$start/$state" "$dir"
	cp "$script" "$dir/TEST.SCR"
	# shellcheck disable=SC2086 # the arguments are words
	set -- $(printf '%b' "$args")
	(ulimit -s 128; "$host" --data "$dir" --lockstep --script TEST.SCR "$@" > /dev/null 2>&1) \
		|| [ $? -lt 128 ] || echo "stack: $name crashed with a 128 KB stack"
done

# Values used before they are set.
for script in "$here"/tests/lockstep/*.scr; do
	name=$(basename "$script" .scr)
	state=$(sed -n 's/^#@start //p' "$script")
	args=$(sed -n 's/^#@args *//p' "$script")
	case $name in si-*) host=$here/build-host-si-vg/u7host ;; *) host=$here/build-host-vg/u7host ;; esac
	dir=$out/vg-$name
	rm -rf "${dir:?}"
	cp -r "$start/$state" "$dir"
	cp "$script" "$dir/TEST.SCR"
	(
		# shellcheck disable=SC2086 # the arguments are words
		set -- $(printf '%b' "$args")
		valgrind --tool=memcheck --track-origins=yes --error-limit=no --log-file="$out/$name.vg" \
			"$host" --data "$dir" --lockstep --script TEST.SCR "$@" > /dev/null 2>&1 || true
	) &
done
wait
python3 -I "$here/tools/valgrind-summary.py" "$out"/*.vg | sed 's/^/valgrind /'

# One line per report: the kind, then the first frame in the game's code.
for log in "$out"/*.log; do
	awk -v scenario="$(basename "$log" .log)" '
		/runtime error:/ { sub(/^.*\/game\//, ""); print "ubsan " $0 " [" scenario "]" }
		/ERROR: AddressSanitizer/ { kind = $3; want = 1; next }
		want && /#[0-9]+ .* in .*\/game\// { sub(/^.* in /, ""); sub(/\/home\/.*\/game\//, ""); print "asan " kind " " $0 " [" scenario "]"; want = 0 }
	' "$log"
done | sort | uniq -c | sort -rn
