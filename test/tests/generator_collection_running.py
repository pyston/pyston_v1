# This test checks if generators which get started but haven't yet stopped (=not raisen a StopIteration exc, etc)
# get freed when there aren't any references to the generators left.

import gc
import weakref

class C(object):
    val = 42

def G():
    l = range(100)
    yield weakref.ref(C())
    while True:
        yield 1

def get_weakrefs(num=5):
    wr = []
    for i in range(num):
        g = G()
        w = g.next()
        wr.append(w)
    return wr

def recurse(f, n):
     if n:
        return recurse(f, n-1)
     return f()

wr = recurse(get_weakrefs, 100)
gc.collect()
for w in wr:
    try:
        print w.__hash__()
        print w().val
    except TypeError as e:
        print e
