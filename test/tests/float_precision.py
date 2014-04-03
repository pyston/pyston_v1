def smallest():
    x = 1.0
    n = 1.0
    while x + n != x:
        n = n * 0.99
    return n

for i in xrange(1000):
    print smallest()

def test():
    BIG = 2.0 ** 53
    t = BIG ** 1.0
    x = 1.0

    n = 10000
    while n:
        t = t + x
        n = n - 1
    return t - BIG

for i in xrange(1000):
    print test()

def divtest(x):
    return (1.0 / x) * x - 1

f = 1.0
while True:
    print divtest(f)
    f = f * 0.999
    if f < 1e-1:
        break
