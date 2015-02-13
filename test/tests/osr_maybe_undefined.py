# statcheck: '-O' in EXTRA_JIT_ARGS or 1 <= stats['num_osr_exits'] <= 5

# Regression test to make sure we can do an OSR if one of the live variables
# is potentially-undefined.

def f(x):
    if x % 2:
        y = x

    for i in xrange(10000):
        pass

    try:
        print y
    except NameError, e:
        print e

f(11)
f(10)
