def f():
    def f1(a, b, c):
        print a, b, c

    f1(1, 2, 3)
    f1(1, b=2, c=3)
    f1(1, b=2, c=3)
    f1(1, c=2, b=3)

    f1(1, b="2", c=3)
    f1(1, b=2, c="3")
    f1(1, c="2", b=3)
    f1(1, c=2, b="3")

    def f2(*args, **kw):
        print args, kw

    f2()
    f2(1)
    f2(1, 2)
    f2((1, 2))
    f2(*(1, 2))
    f2({1:2}, b=2)

    def f3(a=1, b=2, **kw):
        print a, b, kw

    f3(b=3, c=4)
    f3(b=3, a=4)
    f3(b=2, **{'c':3})
f()
f()

# Stress many-arguments:
def f2(a, b=3, c=4, d=5, *args, **kw):
    print a, b, c, d, args, kw

def g():
    try:
        print f2()
    except Exception as e:
        print e
    try:
        print f2(1)
    except Exception as e:
        print e
    try:
        print f2(1, 2)
    except Exception as e:
        print e
    try:
        print f2(1, 2, 3)
    except Exception as e:
        print e
    print f2(1, 2, 3, 4)
    print f2(1, 2, 3, 4, k=1)
    print f2(1, 2, 3, 4, 5)
    print f2(1, 2, 3, 4, 5, 6)
    print f2(1, 2, 3, 4, 5, 6, 7)
    print f2(1, 2, 3, 4, 5, 6, 7, 8)
    print f2(1, 2, 3, 4, 5, 6, 7, 8, 9)
    print f2(1, 2, 3, 4, 5, 6, 7, 8, 9, k=1)

g()
g()
del f2.func_defaults
g()
g()

def f4(a, b, c, d, e):
    print a, b, c, d, e
f4(a=1, b=2, c=3, d=4, e=5)
