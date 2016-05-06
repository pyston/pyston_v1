# Regression test: make sure that, if an exception is thrown after
# rearrange arguments, that we still decref any owned oargs.

def f(a, b, c, d, e, *args):
    print a, b, c, d, e, args
    1/a

for i in xrange(-100, 100):
    try:
        f(i, 0, 0, 0, 0, 0)
    except ZeroDivisionError:
        pass
