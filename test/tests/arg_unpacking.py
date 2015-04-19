def f((a,b)):
    print a,b
    print sorted(list(locals().iteritems()))

f(range(2))
f((1, 2))

def g(a, (b,c), d, (e,f), (g, (h, i), j)):
    print a
    print b
    print c
    print d
    print e
    print f
    print g
    print h
    print i
    print j
    print sorted(list(locals().iteritems()))

g(1, (2, 3), 4, (5, 6), (7, (8, 9), 10))

def h((a,)):
    print a
h((3,))
