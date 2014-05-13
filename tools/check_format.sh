#!/bin/bash
set -eu
for fn in $(find -name '*.cpp' -o -name '*.h'); do
    $1 -style=file -output-replacements-xml $fn | grep -q "replacement offset" && { echo $fn "failed clang-format check"; exit 1; }
done
echo "Format checks passed"
exit 0
