# regression test: list sorting had a gc bug

import gc
class C(object):
    def __init__(self, n):
        self.n = range(n)

    def __eq__(self, rhs):
        # print "eq"
        gc.collect()
        return self.n == rhs.n

    def __lt__(self, rhs):
        # print "lt"
        gc.collect()
        return self.n < rhs.n

def keyfunc(c):
    return c

def f():
    for i in xrange(10):
        print i
        l = [C(i % 5) for i in xrange(10)]
        l.sort(key=keyfunc)
f()
