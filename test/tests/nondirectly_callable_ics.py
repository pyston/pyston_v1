# expected: statfail
# - callable not being patched like this test checks for
#
# run_args: -n
# statcheck: stats['slowpath_runtimecall'] < 10

class Callable(object):
    def __init__(self):
        self.n = 1
    def __call__(self, a, b, c, d, e):
        print self.n, a, b, c, d, e

c = Callable()
for i in xrange(1000):
    c(1, 2, i, 3, 4)

class D(object):
    pass
D.__call__ = c

d = D()
for i in xrange(1000):
    d(1, 2, i, 3, 4)
