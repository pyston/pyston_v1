# This test could really benefit from defined/settable OSR limits

def f(x):
    for i in xrange(20000):
        pass
    yield x

print list(f(5))

