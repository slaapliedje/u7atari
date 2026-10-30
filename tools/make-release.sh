#!/bin/sh
# Builds both games for release and packs them, with release/README.TXT and upstream's licence,
# into dist/u7atari-<version>.zip. The version is the project's in CMakeLists.txt.
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

out=dist/u7atari-$version
rm -rf "${out:?}" "$out.zip"
mkdir -p "$out"
cp build/ULTIMA7.PRG build-si/SERPENT.PRG "$out/"
sed "s/@VERSION@/$version ($(git rev-parse --short HEAD))/" release/README.TXT > "$out/README.TXT"
# TOS has no UTF-8: the licence's curly quotes become plain ones
sed 's/\xe2\x80\x9c/"/g; s/\xe2\x80\x9d/"/g' upstream/LICENSE > "$out/LICENSE.TXT"
unix2dos -q "$out/README.TXT" "$out/LICENSE.TXT"
(cd dist && zip -qr "u7atari-$version.zip" "u7atari-$version")
echo "dist/u7atari-$version.zip"
