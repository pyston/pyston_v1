# To try to make runs with different n as similar as possible, we always
# create an array of N elements by repeating the n as many times as necessary.
# This way, the control flow of the main loop will be the same regardless of
# the number of objects we create -- with such a small benchmark, this ends
# up being noticeable.
M = 64
N = 64

def f(m, n):
    assert N % n == 0
    assert M % m == 0

    l1 = []
    for i in xrange(m):
        class C(object):
            pass
        c = C()
        c.x = i
        l1.append(c)

    lm = []
    while len(lm) < M:
        lm += l1

    ln = []
    while len(ln) < N:
        ln += range(n)

    t = 0
    for _ in xrange(400):
        for i in ln:
            for o in lm:
                if i & 1: t += o.x
                else: t -= o.x
                if i & 2: t += o.x
                else: t -= o.x
                if i & 4: t += o.x
                else: t -= o.x
                if i & 8: t += o.x
                else: t -= o.x
                if i & 16: t += o.x
                else: t -= o.x
                if i & 32: t += o.x
                else: t -= o.x
                if i & 64: t += o.x
                else: t -= o.x
                if i & 128: t += o.x
                else: t -= o.x
                if i & 256: t += o.x
                else: t -= o.x
                if i & 512: t += o.x
                else: t -= o.x
    # print t

import sys
f(int(sys.argv[1]), int(sys.argv[2]))
