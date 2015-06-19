def g(n):
    for i in xrange(n):
        yield n

print len(list(g(2000000)))
