class C(object):
    pass

def f(n):
    c = C()
    c.x = 1.0
    t = 2.0

    for i in xrange(n):
        t = t + (c.x * 1.0 + 1.0)
    # while n:
        # t = t + (c.x * 1.0 + 1.0)
        # n = n - 1
    return t
print f(2000000000)
