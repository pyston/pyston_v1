"""
This script attempts to generate a section_ordering.txt file
based on how gcc decided to lay out the pgo build.
"""

import subprocess
import sys

"""
typical usage:
    make perf_combined
    make pyston_pgo
    python tools/generate_section_ordering_from_pgo_build.py pyston_pgo perf.data > section_ordering.txt
"""

if __name__ == "__main__":
    assert len(sys.argv) == 3, "Usage: %s PGO_BINARY PERF_DATA" % sys.argv[0]
    binary = sys.argv[1]
    perf_data = sys.argv[2]

    perf_results = subprocess.check_output(["perf", "report", "-i", perf_data, "--no-demangle", "-g",
        "none", "--percent-limit", "0.0"])

    functions_in_perf = set([])
    for l in perf_results.split('\n'):
        l = l.strip()
        if not l:
            continue
        if not l[0].isdigit():
            continue
        func_name = l.rsplit(' ', 1)[1]
        if func_name.startswith('0x0'):
            continue
        functions_in_perf.add(func_name)

    print >>sys.stderr, "%d functions in the perf results" % len(functions_in_perf)
    nm_output = subprocess.check_output(["nm", binary]).split('\n')

    functions = []
    for l in nm_output:
        if not l:
            continue
        if not l[0].isalnum():
            continue
        addr, type, name = l.split()
        if type not in 'tT':
            continue

        if name not in functions_in_perf:
            continue

        functions.append((int(addr, 16), name))
    functions.sort()

    for l in functions:
        print ".text." + l[1]
