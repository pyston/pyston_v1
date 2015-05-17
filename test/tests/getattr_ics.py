# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 20

class C(object):
    def f(self):
        self.n

c = C()
c.n = 1

for i in xrange(11000):
    c.f()
    c.n
