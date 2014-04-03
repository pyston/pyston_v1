# "big osr" in terms of lots of live variables needing to be passed through:

def outer(quit):
    if quit:
        return

    a = 1
    b = 2
    c = 3
    d = 4
    e = 5
    f = 6
    g = 7
    h = 8
    i = 9
    l = []

    n = 100000
    while n:
        n = n - 1
        a = a + 1
        b = b + 1
        c = c + 1
        d = d + 1
        e = e + 1
        f = f + 1
        g = g + 1
        h = h + 1
        i = i + 1
        l.append(n)
    print n, a, b, c, d, e, f, g, h, i, len(l)

# Call it a few times to convince it to not be in the interpter:
for i in xrange(10):
    outer(1)
outer(0)
