#!/bin/sh
# Builds both games for release and packs them, with release/README.TXT and upstream's licence,
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
version=$(sed -n 's/^project(u7_atari VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

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
sed "s/@VERSION@/$version ($(git rev-parse --short HEAD))/" release/README.TXT > "$out/README.TXT"
# TOS has no UTF-8: the licence's curly quotes become plain ones
sed 's/\xe2\x80\x9c/"/g; s/\xe2\x80\x9d/"/g' upstream/LICENSE > "$out/LICENSE.TXT"
unix2dos -q "$out/README.TXT" "$out/LICENSE.TXT"
(cd dist && zip -qr "$zipfile" U7ATARI)
echo "dist/$zipfile"
