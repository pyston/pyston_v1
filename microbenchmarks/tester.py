import os
import subprocess
import time

EXECUTABLES = [
    'python',
    os.path.join(os.path.dirname(__file__), '../pyston_pgo'),
    os.path.join(os.path.dirname(__file__), '../../pyston_related/pypy-4.0.0-linux64/bin/pypy')
]

BENCHMARKS = [
    os.path.join(os.path.dirname(__file__), 'megamorphic_control_flow.py')
]
ARGS = [[str(2**i), "16"] for i in xrange(0, 7)]

BENCHMARKS = [
    os.path.join(os.path.dirname(__file__), 'control_flow_ubench.py'),
    os.path.join(os.path.dirname(__file__), 'megamorphic_ubench.py')
]
ARGS = [[str(2**i)] for i in xrange(0, 10)]

NRUNS = 3

for b in BENCHMARKS:
    print os.path.basename(b)
    for args in ARGS:
        print ' '.join(args)
    for exe in EXECUTABLES:
        print os.path.basename(exe)
        for args in ARGS:
            best = float('inf')
            for _ in xrange(NRUNS):
                start = time.time()
                subprocess.check_call([exe, b] + args)
                elapsed = time.time() - start

                best = min(best, elapsed)
            print best
