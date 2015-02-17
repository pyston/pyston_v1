# This test could really benefit from defined/settable OSR limits

def f(x):
    for i in xrange(20000):
        pass
    yield x

print list(f(5))

# Reduced form of the 'permutations' test:
def f2(x):
    if 0:
        a, b, c, d, e = range(5)
    for i in xrange(20000):
        pass
    yield x
    if 0:
        print a, b, c, d, e
print list(f2(5))


# Regression test taken from bm_ai.py:
# Pure-Python implementation of itertools.permutations().
def permutations(iterable, r=None):
    """permutations(range(3), 2) --> (0,1) (0,2) (1,0) (1,2) (2,0) (2,1)"""
    pool = tuple(iterable)
    n = len(pool)
    if r is None:
        r = n
    indices = range(n)
    cycles = range(n-r+1, n+1)[::-1]
    yield tuple(pool[i] for i in indices[:r])
    while n:
        for i in reversed(range(r)):
            cycles[i] -= 1
            if cycles[i] == 0:
                indices[i:] = indices[i+1:] + indices[i:i+1]
                cycles[i] = n - i
            else:
                j = cycles[i]
                indices[i], indices[-j] = indices[-j], indices[i]
                yield tuple(pool[i] for i in indices[:r])
                break
        else:
            return

t = 0
for p in permutations(range(10), 3):
    t += 1
print t

