# skip-if: True

# This is a test to make sure that the stack space we allocate
# for "long arg" calls (ie calls that take more than 4 arguments)
# gets restored.

def f6(a1, a2, a3, a4, a5, a6, l1, l2, l3, l4, l5, l6, x1, x2, x3, x4, x5, x6):
    return 1

def f():
    a = 1
    b = 2
    c = 3
    d = 4
    e = 5

    n = 100000000
    t = 0

    l1 = []
    l2 = []
    l3 = []
    l4 = []
    l5 = []
    l6 = []
    while n:
        t = t + f6(l1, l2, l3, l4, l5, l6, l1, l2, l3, l4, l5, l6, l1, l2, l3, l4, l5, l6)
        n = n - 1
        if n % 10000 == 0:
            print n

    print a, b, c, d, e
f()
