def f():
    u = u"a" * 100
    c = unicode
    for i in xrange(2000000):
        c(u)
f()
