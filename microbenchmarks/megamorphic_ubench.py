# To try to make runs with different n as similar as possible, we always
# create an array of N elements by repeating the n as many times as necessary.
# This way, the control flow of the main loop will be the same regardless of
# the number of objects we create -- with such a small benchmark, this ends
# up being noticeable.
N = 1024

def f(n):
    l = []
    assert N % n == 0

    l2 = []
    for i in xrange(n):
        class C(object):
            pass
        c = C()
        c.x = i
        l2.append(c)

    while len(l) < N:
        l += l2

    t = 0
    for _ in xrange(20000):
        for o in l:
            t += o.x

import sys
f(int(sys.argv[1]))
