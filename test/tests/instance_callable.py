class C(object):
    def __init__(self, f):
        self.f = f

def foo(n):
    print "foo", n

c = C(foo)
for i in xrange(100):
    c.f(i)
