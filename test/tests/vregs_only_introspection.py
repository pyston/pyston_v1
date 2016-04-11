import sys
def f():
    # By the time of the f_locals call, x will only be alive because
    # of its existence in the vregs array.
    x = 100.0 ** 10
    for i in xrange(10000):
        200.0 * 200.0
    print sorted(sys._getframe(0).f_locals.items())

    # Avoid testing the lifetime of i by keeping it alive here:
    print i
f()

# Same test but with ints.  Ints are harder to debug since they use a freelist
# and it isn't as obvious what is going on.
def f():
    y = 100 * 100
    for i in xrange(10000):
        200 * 200
    print sorted(sys._getframe(0).f_locals.items())

    # Avoid testing the lifetime of i by keeping it alive here:
    print i
f()
