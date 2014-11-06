#!/usr/bin/env python

import commands
import os.path
import subprocess
import time

import submit

def run_tests(executables, benchmarks, callback):
    times = [[] for e in executables]

    for b in benchmarks:
        for e, time_list in zip(executables, times):
            start = time.time()
            subprocess.check_call(e.args + [b], stdout=open("/dev/null", 'w'))
            elapsed = time.time() - start

            print "%s %s: % 4.1fs" % (e.name.rjust(15), b.ljust(35), elapsed)

            time_list.append(elapsed)

            if callback:
                callback(e, b, elapsed)

    for e, time_list in zip(executables, times):
        t = 1
        for elapsed in time_list:
            t *= elapsed
        t **= (1.0 / len(time_list))
        print "%s %s: % 4.1fs" % (e.name.rjust(15), "geomean".ljust(35), t)


class Executable(object):
    def __init__(self, args, name):
        self.args = args
        self.name = name

def main():
    executables = [Executable(["./pyston_release", "-q"], "pyston")]

    RUN_CPYTHON = 0
    if RUN_CPYTHON:
        executables.append(Executable(["python"], "cpython 2.7"))
    DO_SUBMIT = 1
    # if RUN_PYPY:
        # executables.append(Executable(["python"], "cpython 2.7"))

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

    GIT_REV = commands.getoutput("git rev-parse HEAD")
    def submit_callback(exe, benchmark, elapsed):
        benchmark = os.path.basename(benchmark)

        assert benchmark.endswith(".py")
        benchmark = benchmark[:-3]

        commitid = GIT_REV
        if "cpython" in exe.name:
            commitid = "default"
        submit.submit(commitid=commitid, benchmark=benchmark, executable=exe.name, value=elapsed)

    callback = None
    if DO_SUBMIT:
        callback = submit_callback

    run_tests(executables, benchmarks, callback)

if __name__ == "__main__":
    main()
