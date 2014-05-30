
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

    class C(B):
        def foo(self, n):
            print n

    c = C()
    print c.wrapper(2)
f2()
