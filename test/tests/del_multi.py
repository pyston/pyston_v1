def f4():
    a = 1
    b = 2
    del [(a,), b]
    if 0:
        print a, b
    print locals()
f4()

class C(object):
    pass
def f5():
    c = C()
    c.a = 1
    c.b = 2
    c.c = 3
    l = range(5)
    del ([c.a], [c.b], l[3])
    print hasattr(c, "a")
    print hasattr(c, "b")
    print hasattr(c, "c")
    print l
f5()
