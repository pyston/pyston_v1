# Try to trick the JIT into OSR'ing into an optimized version with a speculation
# that has already failed.
# In the f() function, y will have type int, but when we OSR from the while loop,
# we'll calculate the types as if xrange() had returned an xrange object.  We'll
# have to emit a guard for that and branch to a deopt version.

def xrange(n):
    return n

def f(x):
    y = xrange(x)

    n = 100000
    while n:
        n -= 1

    print y
    print y + 1

f(11)
f(10)
