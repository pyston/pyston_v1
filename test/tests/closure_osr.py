# This test could really benefit from defined/settable OSR limits

def f(x):
    def inner():
        t = 0
        for i in xrange(20000):
            t += x
        return t

    return inner

f = f(5)
print f()

