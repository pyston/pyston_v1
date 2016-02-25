N = 1024
def f(n):
    l = []
    assert N % n == 0

    while len(l) < N:
        l += range(n)

    t = 0
    for _ in xrange(2000):
        for i in l:
            if i & 1: t += 1
            else: t += 2
            if i & 2: t += 1
            else: t += 2
            if i & 4: t += 1
            else: t += 2
            if i & 8: t += 1
            else: t += 2
            if i & 16: t += 1
            else: t += 2
            if i & 32: t += 1
            else: t += 2
            if i & 64: t += 1
            else: t += 2
            if i & 128: t += 1
            else: t += 2
            if i & 256: t += 1
            else: t += 2
            if i & 512: t += 1
            else: t += 2
    # print t

import sys
f(int(sys.argv[1]))
