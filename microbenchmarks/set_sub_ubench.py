l = [set(range(5)) for i in xrange(1000)]
def f():
    s1 = set(range(1))
    s2 = set(range(1))
    for i in xrange(400000):
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
        s1 - s2
f()
