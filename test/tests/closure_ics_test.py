# Regression test: this triggers a bug in the way we guard for boxedfunctions and their closures.
# In particular, we guard on the specific value of a BoxedFunction to say that it is the same, but
# it's possible for the function to get destructed and then a new (and different) boxedfunction to
# get allocated in the same address, which would pass the guard.

import gc

def f():
    x = []
    def inner():
        x.append(1)
    inner()

for i in xrange(30000):
    if i % 1000 == 0:
        print i
    f()
    if i % 50 == 0:
        gc.collect()
