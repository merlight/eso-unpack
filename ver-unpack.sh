#! /bin/sh

set -e

die () {
    printf >&2 "%s\n" "$*"
    exit 1
}

src="${1%/}"
ver=$(tail -1 "$src/depot/_databuild/databuild.stamp") ||
    die "could not determine game version"

dst="unpacked-$src-$ver"
tags="$dst/tags"

test -f build/Makefile ||
(
    mkdir -p build
    cd build && cmake ..
)
make -C build

printf "unpacking %s\n" "$dst"
build/eso-unpack --save --esodir "$src" --outdir "$dst" 2>build/err >build/out
find "$dst/" -empty -delete

printf "generating %s\n" "$tags"
ctags -Rf "$tags" --options=ctags.conf --totals "$(readlink -m "$dst")"
ln -sfT "$tags" "tags"
