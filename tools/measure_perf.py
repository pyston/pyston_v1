#!/usr/bin/env python

import subprocess
import time

def run_tests(executables, benchmarks):
    times = [[] for e in executables]

    for b in benchmarks:
        for e, time_list in zip(executables, times):
            start = time.time()
            subprocess.check_call(e + [b], stdout=open("/dev/null", 'w'))
            elapsed = time.time() - start

            print "%s %s: % 4.1fs" % (" ".join(e).rjust(25), b.ljust(35), elapsed)

            time_list.append(elapsed)

    for e, time_list in zip(executables, times):
        t = 1
        for elapsed in time_list:
            t *= elapsed
        t **= (1.0 / len(time_list))
        print "%s %s: % 4.1fs" % (" ".join(e).rjust(25), "geomean".ljust(35), t)



def main():
    executables = [["./pyston_release", "-q"]]

    benchmarks = []

    benchmarks += ["../microbenchmarks/%s" % s for s in [
        ]]

    benchmarks += ["../minibenchmarks/%s" % s for s in [
        "fannkuch_med.py",
        "nbody_med.py",
        "interp2.py",
        "raytrace.py",
        "chaos.py",
        "nbody.py",
        "fannkuch.py",
        "spectral_norm.py",
        ]]

    run_tests(executables, benchmarks)

if __name__ == "__main__":
    main()
