class C(object):
    pass

def f():
    g = getattr
    c = C()
    c.o = 1
    for i in xrange(1000000):
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
        g(c, "o")
f()
