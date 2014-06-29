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
