
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
