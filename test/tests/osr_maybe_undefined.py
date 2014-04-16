# Regression test to make sure we can do an OSR if one of the live variables
# is potentially-undefined.

def f(x):
    if x % 2:
        y = x

    for i in xrange(1000000):
        pass

    print y

f(11)
f(10)
