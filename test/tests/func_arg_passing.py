def f(a, b, c):
    print a, b, c

f(1, 2, 3)
f(1, b=2, c=3)
f(1, b=2, c=3)
f(1, c=2, b=3)

f(1, b="2", c=3)
f(1, b=2, c="3")
f(1, c="2", b=3)
f(1, c=2, b="3")

def f(*args, **kw):
    print args, kw

f()
f(1)
f(1, 2)
f((1, 2))
f(*(1, 2))
f({1:2}, b=2)

def f(a=1, b=2, **kw):
    print a, b, kw

f(b=3, c=4)
f(b=3, a=4)
