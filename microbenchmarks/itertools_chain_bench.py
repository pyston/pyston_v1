# simple benchmark to test iteration over extension objects

import itertools

def f(c):
    for i in c:
        pass

l = []
for i in xrange(100):
    l.append(itertools.chain(*[range(500) for j in xrange(500)]))
c = itertools.chain(*l)
f(c)
