def g():
    l1 = [1]
    l2 = [2]
    l3 = [3]
    l4 = [4]
    l5 = [5]
    l6 = [6]
    l7 = [7]
    l8 = [8]
    l9 = [9]
    l10 = [10]
    l11 = [11]
    l12 = [12]
    l13 = [13]

    yield 1

    print l1, l2, l3, l4, l5, l6, l7, l8, l9, l10, l11, l12, l13

    yield 2

g = g()

print g.next()
l = [None] * 10
for i in xrange(1000):
    l * 1000

print g.next()
