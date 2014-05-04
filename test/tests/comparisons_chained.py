# expected: fail
# - chained comparisons
# - "in" and "not in"

def f(n):
    print "f(%d)" % n
    return n

# f(3) shouldn't get called:
f(1) <= f(2) < f(1) < f(3)
