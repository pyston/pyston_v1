def f2(i):
    return 1.0 / (i - 5)

def f1():
    # save some locals:
    a, b, c, d, e, f, g = range(7)

    for i in xrange(10):
        try:
            print i, f2(i)
        except Exception, e:
            print e

    return a, b, c, d, e, f, g

print f1()
