# Regression test to make sure we can do an OSR if one of the live variables
# is potentially-undefined.

def f(x):
    if x % 2:
        y = x

    xrange(0)

    for i in xrange(1000000):
        xrange(0)

    print y
    xrange(0)

f(11)
f(10)
