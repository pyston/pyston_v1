# Regression test: promoting an UNDEF to a known type

def f():
    match = None # This could be a function that usually returns None but sometimes returns a string
    if 0:
        s = match.foo() or ""
        print s
for i in xrange(10000):
    f()
