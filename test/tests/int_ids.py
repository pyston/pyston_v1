# Regression test:
# speculation should not result in ints changing their ids.
# I guess this means that if we unbox, we have to know what we would box to.

class C(object):
    pass

c = C()
c.a = 100000

# Test it via osr:
def f():
    for i in xrange(11000):
        a1 = c.a
        a2 = c.a
        if 0:
            pass
        assert id(a1) == id(a2), i
f()

# Test it via reopt:
def f2():
    a1 = c.a
    a2 = c.a
    if 0:
        pass
    assert id(a1) == id(a2)
for i in xrange(11000):
    f2()

# Test function returns:
def g():
    return 1000
def f3():
    assert id(g()) == id(g())
for i in xrange(11000):
    f3()

# Test function args:
def f4(a, b):
    assert id(a) == id(b)
for i in xrange(11000):
    f4(1000, 1000)

def f6():
    for i in xrange(11000):
        a = b = 1000
        if True:
            pass
        f4(a, b)
f6()


# This applies to other data types as well.  (maybe should call this test file something else)
def ident(x):
    return x

def f5():
    t = (1, 2, 3)
    assert ident(t) is t

for i in xrange(10):
    f5()
