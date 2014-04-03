# run_args: -n
# statcheck: stats['slowpath_getattr'] <= 10

class C(object):
    def f(self):
        print self.n

c = C()
c.n = 1

for i in xrange(100):
    c.f()
