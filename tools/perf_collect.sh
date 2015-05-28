#!/bin/sh

set -e

if [ "$#" -eq 0 ] ; then
    echo "Usage: perf_collect.sh [commands to run benchmark]"
    exit 1
fi

perf record -o $1.data -g -- $@
perf report -i $1.data -n --no-call-graph | bash tools/cumulate.sh > $1.txt
echo "Report saved in $1.txt (raw perf data saved in $1.data)"
