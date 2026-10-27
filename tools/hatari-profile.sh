#!/bin/sh
# Profiles the game under Hatari and prints where the CPU time went, by function.
#   tools/hatari-profile.sh <tt|falcon> <hd-dir> <start> <vbls> [symbol]
# Runs tools/hatari-test.sh on <hd-dir> (set up as for that script), turns Hatari's CPU profiler
# on at VBL <start> and stops after <vbls> more. With [symbol] (e.g. _plat_pcm_start), profiling
# starts instead the first time the program reaches that function after VBL <start>.
# Leaves the log, the profile, and a screenshot from the end of the profile in
# $U7_PROFILE_DIR (default: a new temporary directory). Counts come from Hatari's emulation,
# whose timing is approximate: compare runs with each other, not with real hardware.
set -eu
machine=$1 hd=$2 start=$3 vbls=$4 symbol=${5:-}
here=$(cd "$(dirname "$0")/.." && pwd)
out=${U7_PROFILE_DIR:-$(mktemp -d)}
mkdir -p "$out"
prg=${U7_PRG:-$here/build/ULTIMA7.PRG}

if [ -n "$symbol" ]; then
	printf 'b VBL = %s :once :trace :file %s/arm.ini\n' "$start" "$out" > "$out/init.ini"
	printf 'symbols prg\nb pc = %s :once :trace :file %s/start.ini\n' "$symbol" "$out" > "$out/arm.ini"
else
	printf 'b VBL = %s :once :trace :file %s/start.ini\n' "$start" "$out" > "$out/init.ini"
fi
printf 'profile on\nb VBL = "VBL+%s" :once :file %s/stop.ini\n' "$vbls" "$out" > "$out/start.ini"
printf 'screenshot %s/end.png\ne TEXT\nprofile save %s/profile.txt\nquit\n' "$out" "$out" > "$out/stop.ini"

# Hatari sometimes stays up after the quit; stop it once the profile is out.
U7_PRG=$prg U7_TIMEOUT=${U7_TIMEOUT:-3600} "$here/tools/hatari-test.sh" "$machine" "$hd" 1000000 \
	--parse "$out/init.ini" > "$out/hatari.log" 2>&1 &
runner=$!
while kill -0 "$runner" 2>/dev/null && ! grep -q '^> quit' "$out/hatari.log"; do
	sleep 2
done
sleep 2
for pid in $(pgrep -f "hatari .*--parse $out/init.ini" || true); do
	kill "$pid" 2>/dev/null || true
done
wait "$runner" || true

text=$(sed -n '/^> e TEXT/{n;s/.*\$\([0-9a-f]*\) (hex).*/\1/p;}' "$out/hatari.log")
if [ -z "$text" ]; then
	echo "no profile: see $out/hatari.log" >&2
	exit 1
fi
m68k-atari-mint-nm -n "$prg" > "$out/symbols.txt"
echo "profile in $out"
python3 "$here/tools/profile-funcs.py" "$out/hatari.log" "$out/symbols.txt" "$text" "${U7_PROFILE_ROWS:-30}"
