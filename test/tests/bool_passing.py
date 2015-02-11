# Test that we can pass known-bool values through OSRs and what not:

def f(b):
    b2 = b.__nonzero__()
    for i in xrange(10000):
        pass
    print b, b2

f(True)
