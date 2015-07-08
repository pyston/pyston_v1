class C(object):
    pass

def f():
    g = getattr
    c = C()
    c.o = 1
    for i in xrange(10000000):
        g(c, "o")
f()
