# expected: fail
# - chained comparisons
# - "in" and "not in"

def f(n):
    print "f(%d)" % n
    return n

f(1) <= f(2) < f(3)

for i in xrange(1, 4):
    print i in range(6), i not in range(5)
