# Test various ways that we use "scratch" space.
# Tests some regressions where we weren't correctly counting those as reference uses.

def f(arg=2.0**4):
    print arg
f()

def f2(a, b, c, d):
    print a
    print b
    print c
    print d
f2(2.0**5, 2.0**6, 2.0**7, 2.0**8)

def f3():
    t = (2.0**9, 2.0**10, 2.0**11, 2.0**12, 2.0**13)
    print t
f3()
