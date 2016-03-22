#! /bin/bash

grep --recursive --no-filename --text --only-matching --ignore-case \
    --include="*."{lua,txt,unk,xml,zosft} \
    'esoui[/0-9a-z_]*[.]dds' "$@" \
    | tr 'A-Z' 'a-z' \
    | sort -u

