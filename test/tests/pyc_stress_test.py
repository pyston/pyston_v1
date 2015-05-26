# skip-if: '-x' in EXTRA_JIT_ARGS
# - too slow

# Note: CPython doesn't pass this test

import os
import sys
import multiprocessing

def worker():
    global done

    for i in xrange(1000):
        del sys.modules["pyc_import_target"]
        import pyc_import_target

    done = True

import pyc_import_target
path = os.path.join(os.path.dirname(__file__), "pyc_import_target.pyc")
assert os.path.exists(path)

TEST_THREADS = 3

l = []
for i in xrange(TEST_THREADS):
    p = multiprocessing.Process(target=worker)
    p.start()
    l.append(p)

idx = 0
while l:
    p = l.pop()
    while p.is_alive():
        for i in xrange(100):
            if os.path.exists(path):
                os.remove(path)
        for i in xrange(100):
            if os.path.exists(path):
                with open(path, "rw+") as f:
                    f.write(chr(i) * 100)
                    f.truncate(200)
    p.join()
    assert p.exitcode == 0, p.exitcode
