def f(o, msg):
    print msg
    return o

g1 = (f(i, i) for i in f(xrange(5), "xrange"))
print 1
print g1.next()
print list(g1)

print
def f2():
    g2 = (f(i, j) for i in f(xrange(4), "inner xrange") if i != f(2, 2) if i != f(20, 20) for j in f(xrange(4), "outer xrange") if i % 2 == j % 2)
    print 1
    print g2.next()
    print list(g2)
f2()

