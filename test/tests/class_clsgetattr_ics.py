# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 10

class C(object):
    pass


c = C()
c.n = 1
C.m = 2

for i in xrange(1000):
    print c.n, c.m
