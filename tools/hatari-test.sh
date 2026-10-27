#!/bin/sh
# Runs build/ULTIMA7.PRG headless under Hatari and prints what the program wrote to the console.
#   tools/hatari-test.sh <tt|falcon> <hd-dir> [vbls] [extra hatari options...]
# <hd-dir> becomes drive C: and must hold the game in U7\ (STATIC\ and U7.CFG); the PRG is
# copied there first (U7_PRG names another build's PRG). Use a copy of the game: the run can
# write saves into it.
# MIDI is off: Hatari otherwise loads the user's own hatari.cfg, MIDI device included, and the
# test would play into a real synth. U7_TEST_MIDI=1 turns it back on. U7_TIMEOUT=<seconds> stops
# Hatari itself after that long (a timeout around this script would leave it running).
set -eu
machine=$1 hd=$2 vbls=${3:-1500}
shift 2; [ $# -gt 0 ] && shift
here=$(cd "$(dirname "$0")/.." && pwd)
cp "${U7_PRG:-$here/build/ULTIMA7.PRG}" "$hd/U7/ULTIMA7.PRG"
cfg=$(mktemp); trap 'rm -f "$cfg"' EXIT

case $machine in
tt)     set -- --machine tt -s 4 --ttram 64 "$@" ;;
falcon) set -- --machine falcon -s 14 --ttram 128 --cpuclock 32 --fpu 68882 --mic off "$@" ;;
*)      echo "machine must be tt or falcon" >&2; exit 2 ;;
esac

# --addr24 off: with TT-RAM, Hatari otherwise stops on an alert no one can click headless.
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=dummy \
${U7_TIMEOUT:+timeout "$U7_TIMEOUT"} hatari -c "$cfg" --tos /usr/share/hatari/etos512us.img --addr24 off --monitor vga \
	--drive-a off --drive-b off -d "$hd" --auto 'C:\U7\ULTIMA7.PRG' --conout 2 \
	--fast-forward on --sound off --midi "$( [ "${U7_TEST_MIDI:-0}" = 1 ] && echo on || echo off)" \
	--run-vbls "$vbls" --log-level fatal "$@" 2>&1 \
	| stdbuf -o0 tr -d '\033' | grep --line-buffered -v '^\s*$' || true
