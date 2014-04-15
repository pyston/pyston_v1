# Regression test to make sure we can do an OSR if one of the live variables
# is potentially-undefined.

def f(x):
    if x:
        y = 1

    for i in xrange(10000000):
        pass

    print y
f(1)
f(0)
