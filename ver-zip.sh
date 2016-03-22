#! /bin/bash

set -e

die () {
    printf >&2 "%s\n" "$*"
    exit 1
}

test -n "$1" || die "missing argument: unpacked-VERSION directory"
test -z "${1##unpacked-*}" || die "invalid argument: unpacked-VERSION directory required"

cd "$1"

ver="${1%/}"
ver="${ver#unpacked-}"

rm -f "/tmp/esoui-$ver.zip"
7z a -x'!'esoui/{art,common/fonts}/ "/tmp/esoui-$ver.zip" esoui/
