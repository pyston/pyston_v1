# expected: reffail
# Test to make sure that generators create and receive closures as appropriate.

def f(E, N, M):
    print list((i**E for i in xrange(N) for j in xrange(M)))

f(4, 3, 2)


def f2(x):
    yield list((list(i for i in xrange(y)) for y in xrange(x)))
print list(f2(4))

def f3(z):
    print list((lambda x: x**y)(z) for y in xrange(10))
f3(4)

# Generator-closure handling also needs to handle when the closures
# are at the module scope:
n = 5
g1 = (i for i in xrange(n))
