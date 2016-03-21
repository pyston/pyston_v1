# Test a generator cycle involving an unfinished generator.

def f():
    g = (i in (None, g) for i in xrange(2))
    print g.next()
print f()
