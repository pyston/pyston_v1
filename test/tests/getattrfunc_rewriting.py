# Regression test: make sure we guard / invalidate our getattr() rewrites:

class C(object):
    pass

c = C()
for i in xrange(100):
    setattr(c, "attr%d" % i, i)

def f():
    for j in xrange(2, 10):
        t = 0
        for i in xrange(2000):
            for k in xrange(j):
                t += getattr(c, "attr%d" % k)
        print t
f()
