# statcheck: '-L' in EXTRA_JIT_ARGS or 1 <= stats['num_osr_exits'] <= 5

# Regression test to make sure we can do an OSR if one of the live variables
# is potentially-undefined.

def f(x):
    if x % 2:
        y = x

    xrange(0)

    for i in xrange(10000):
        xrange(0)

    try:
        print y
    except NameError, e:
        print e
    xrange(0)

f(11)
f(10)
