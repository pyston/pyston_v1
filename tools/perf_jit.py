#!/usr/bin/python2
import sys, subprocess

if "/tmp/perf-" not in sys.argv[-1]:
    subprocess.check_call(["objdump"] + sys.argv[1:])
else:
    for arg in sys.argv:
        if "--start-address=" in arg:
            start_addr = int(arg[len("--start-address="):], 16)
    subprocess.check_call(["python", "tools/annotate.py", "--no-print-raw-bytes", "--no-print-perf-counts", "0x%x" % start_addr])
