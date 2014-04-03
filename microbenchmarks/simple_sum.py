def f(n):
    t = 0
    for i in xrange(n):
        t = t + n
    return t
print f(1000000)

_xrange = xrange
def xrange(end):
    return _xrange(end-1)
print f(1000000)
