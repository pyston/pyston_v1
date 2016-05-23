#!/usr/bin/env bash
set -eu
failed=0
for fn in $(find . -path ./core/from_llvm -prune -o -name '*.cpp' -print -o -name '*.h' -print); do
    diff -u $fn <($1 -style=file $fn) || failed=1
done

if [ $failed -eq 0 ]; then
    echo "Format checks passed"
    exit 0
else
    echo "Format checks failed; please run 'make format'"
    exit 1
fi
