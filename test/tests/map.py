def f(x):
    print "f(%s)" % x
    return -x

print map(f, range(10))
