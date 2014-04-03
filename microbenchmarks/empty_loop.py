def f(n):
    for i in xrange(n):
        pass
f(100000000)
print "done with first"

_xrange = xrange
def xrange(end):
    return _xrange(end-1)
f(100000000)
print "done with second"
