# fail-if: '-n' not in EXTRA_JIT_ARGS and '-O' not in EXTRA_JIT_ARGS
# - This test needs type profiling to be enabled to trigger the bug

# This test throws an irgen assertion.
# The issue is that type analysis is stronger than phi analysis and
# they produce slightly different results.

import thread
l = thread.allocate_lock()

class L(object):
    def __enter__(self):
        return self
    def __exit__(self, *args):
        pass
L = thread.allocate_lock

class C(object):
    def __init__(self):
        self.lock = L()
    def f(self):
        try:
            with self.lock:
                for i in []:
                    pass
        finally:
            pass

c = C()
for i in xrange(10000):
    c.f()
