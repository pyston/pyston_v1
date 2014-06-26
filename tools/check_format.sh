#!/bin/sh
set -eu
failed=0
for fn in $(find . -name '*.cpp' -o -name '*.h'); do
    $1 -style=file -output-replacements-xml $fn | grep -q "replacement offset" && { echo $fn "failed clang-format check"; failed=1; }
done

if [ $failed -eq 0 ]; then
    echo "Format checks passed"
    exit 0
else
    exit 1
fi
