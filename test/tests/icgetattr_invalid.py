# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 10
# Different ways that getattrs can be invalidated

class C(object):
    pass

def f(o):
    print o.n

c = C()
c.n = 1
f(c)

def ga(o, attr):
    print "in ga()"
    return 2
# Setting __getattribute__ invalidates all ics:
C.__getattribute__ = ga
f(c)


# Here we get a class attr, which should also be cached,
# but there are a few more ways that it can be invalidated.
# Also, test setting non-functions
class C(object):
    def f(self):
        print "f()"
class Callable(object):
    def __init__(self, n):
        self.n = n
    def __call__(self):
        print "Callable", self.n

def new_f(self):
    print "new_f"

c = C()
for i in xrange(1000):
    print i
    c.f()
    if i == 100:
        C.f = Callable(i)
    if i == 150:
        C.z0 = 1
    if i == 200:
        C.f = new_f
    if i == 250:
        C.z1 = 2
    if i == 300:
        c.f = Callable(i)
    if i == 350:
        C.z2 = 3
