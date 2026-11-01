#!/bin/sh
# Builds both games for release and packs them, with release/README.TXT, tools/exult2u7.py and the port's and upstream's licences,
# into dist/u7atari-<version>.zip, in a folder U7ATARI. The version is the project's in CMakeLists.txt.
#   tools/make-release.sh
# Uses (and configures, when missing) the release build folders build and build-si. Refuses to
# run with uncommitted changes, so a release matches a commit.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

if [ -n "$(git status --porcelain)" ] || [ -n "$(git -C upstream status --porcelain)" ]; then
	echo "make-release: uncommitted changes; commit them first" >&2
	exit 1
fi
version=$(sed -n 's/^project(u7atari VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

build() {
	dir=$1; shift
	[ -f "$dir/CMakeCache.txt" ] || cmake -B "$dir" -DCMAKE_TOOLCHAIN_FILE=cmake/m68k-atari-mint.cmake \
		-DCMAKE_BUILD_TYPE=Release "$@"
	cmake --build "$dir" -j
}
build build
build build-si -DU7_GAME="Serpent Isle"

# the folder inside has an 8.3 name, for unpacking on the Atari
out=dist/U7ATARI
zipfile=u7atari-$version.zip
rm -rf "${out:?}" "dist/$zipfile"
mkdir -p "$out"
cp build/ULTIMA7.PRG build-si/SERPENT.PRG "$out/"
cp tools/exult2u7.py "$out/EXULT2U7.PY"
sed "s/@VERSION@/$version ($(git rev-parse --short HEAD))/" release/README.TXT > "$out/README.TXT"
# the port's licence, then upstream's; TOS has no UTF-8, so curly quotes become plain ones
{
	echo "U7ATARI (platform/atari and the build)"; echo
	cat LICENSE; echo; echo
	echo "THE DECOMPILATION BY JAMIE BLANKS (Kitrinx/Ultima7_Decomp, ultima7-portable)"; echo
	cat upstream/LICENSE
} | sed 's/\xe2\x80\x9c/"/g; s/\xe2\x80\x9d/"/g' > "$out/LICENSE.TXT"
unix2dos -q "$out/README.TXT" "$out/LICENSE.TXT"
(cd dist && zip -qr "$zipfile" U7ATARI)
echo "dist/$zipfile"
