# run_args: -n
# statcheck: noninit_count('slowpath_runtimecall') < 10

class C(object):
    def foo(self, a, b, c, d, e):
        print a, b, c, d, e

c = C()
for i in xrange(1000):
    c.foo(1, i, 3, i, 5)
