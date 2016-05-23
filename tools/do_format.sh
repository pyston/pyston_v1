#!/usr/bin/env bash
set -eu
for fn in $(find . -path ./core/from_llvm -prune -o -name '*.cpp' -print -o -name '*.h' -print); do
    $1 -i --style=file $fn
done
