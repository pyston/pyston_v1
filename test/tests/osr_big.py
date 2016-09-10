# statcheck: '-L' in EXTRA_JIT_ARGS or 1 <= stats['num_osr_exits'] <= 5

# "big osr" in terms of lots of live variables needing to be passed through:

try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_INTERPRETER", 5)
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 5)
except ImportError:
    pass

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

    n = 10000
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
