# To try to make runs with different n as similar as possible, we always
# create an array of N elements by repeating the n as many times as necessary.
# This way, the control flow of the main loop will be the same regardless of
# the number of objects we create -- with such a small benchmark, this ends
# up being noticeable.
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
