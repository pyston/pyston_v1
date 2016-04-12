def f1():
    class C(object):
        pass

    class D(C):
        pass

    C.a = 1
    print D.a

    for inst in [C(), D(), object()]:
        for cls in [C, D, object]:
            print isinstance(inst, cls)

    print object.__base__
    print C.__base__
    print D.__base__
    print type.__base__
    print type(None).__base__

    # Just make sure these exist:
    repr(Exception.__base__)
    repr(str.__base__)
f1()

def f2():
    class B(object):
        def wrapper(self, n):
            self.foo(n)
            self.foo(n)

        def foo(self, n):
            print "B.foo()", n

    class C(B):
        def foo(self, n):
            print "C.foo()", n

    print B().wrapper(2)
    print C().wrapper(2)
f2()

def f3():
    class I(int):
        def foo(self):
            print self + 1

    a = I(1)
    b = I(2)
    print a, type(a)
    print b, type(b)
    c = a + b
    print c, type(c)
    d = c + a
    e = a + c
    print d, type(d)
    print e, type(e)
    f = +a
    print f, type(f)

    print a.foo()
f3()

def f4():
    print
    print "f4"

    class C(object):
        A = 1
        def __init__(self, n):
            super(C, self).__init__()
            self.n = n

        def foo(self):
            print "C.foo()", self.n

    class D(C):
        A = 2
        def __init__(self, n, m):
            super(D, self).__init__(n)
            self.m = m

        def foo(self):
            super(D, self).foo()
            print "D.foo()", self.m

    c = C(1)
    d = D(1, 2)
    c.foo()
    d.foo()
    C.foo(c)
    C.foo(d)

    print c.A
    print d.A
    print super(D, d).A
f4()

print isinstance(1, int)
print isinstance(1, object)
print isinstance(1, float)
print issubclass(int, int)
print issubclass(int, object)
print issubclass(int, float)
