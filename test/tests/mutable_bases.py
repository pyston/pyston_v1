class A(object):
    def foo(self):
        print "foo"

class B(object):
    def bar(self):
        print "bar"

class C(object):
    def baz(self):
        print "baz"

class D(C):
    pass
print D.__bases__ == (C,)
print hasattr(D, "bar")
try:
    D.__bases__ += (A, B, C)
except Exception as e:
    print e # duplicate base class C
D.__bases__ += (A, B)
print D.__bases__ == (C, A, B, C)
print D.__base__ == (C)
D().foo(), D().bar(), D().baz()
D.__bases__ = (C,)
print D.__bases__ == (C,)
print hasattr(D, "foo"), hasattr(D, "bar"), hasattr(D, "baz")
D().baz()

# inheritance circle:
try:
    C.__bases__ = (D,)
except TypeError as e:
    print e

class Slots(object):
    __slots__ = ["a", "b", "c"]
    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3
try:
    Slots.__bases__ = (A,)
except TypeError:
    print "cought TypeError exception" # pyston and cpython throw a different exception

# This tests are copied from CPython an can be removed when we support running test/cpython/test_descr.py
class CPythonTests(object):
    def assertEqual(self, x, y):
        assert x == y
    def fail(self, msg):
        print "Error", msg

    def test_mutable_bases(self):
        # Testing mutable bases...

        # stuff that should work:
        class C(object):
            pass
        class C2(object):
            def __getattribute__(self, attr):
                if attr == 'a':
                    return 2
                else:
                    return super(C2, self).__getattribute__(attr)
            def meth(self):
                return 1
        class D(C):
            pass
        class E(D):
            pass
        d = D()
        e = E()
        D.__bases__ = (C,)
        D.__bases__ = (C2,)
        self.assertEqual(d.meth(), 1)
        self.assertEqual(e.meth(), 1)
        self.assertEqual(d.a, 2)
        self.assertEqual(e.a, 2)
        self.assertEqual(C2.__subclasses__(), [D])

        try:
            del D.__bases__
        except (TypeError, AttributeError):
            pass
        else:
            self.fail("shouldn't be able to delete .__bases__")

        try:
            D.__bases__ = ()
        except TypeError, msg:
            if str(msg) == "a new-style class can't have only classic bases":
                self.fail("wrong error message for .__bases__ = ()")
        else:
            self.fail("shouldn't be able to set .__bases__ to ()")

        try:
            D.__bases__ = (D,)
        except TypeError:
            pass
        else:
            # actually, we'll have crashed by here...
            self.fail("shouldn't be able to create inheritance cycles")

        try:
            D.__bases__ = (C, C)
        except TypeError:
            pass
        else:
            self.fail("didn't detect repeated base classes")

        try:
            D.__bases__ = (E,)
        except TypeError:
            pass
        else:
            self.fail("shouldn't be able to create inheritance cycles")

        # let's throw a classic class into the mix:
        class Classic:
            def meth2(self):
                return 3

        D.__bases__ = (C, Classic)

        self.assertEqual(d.meth2(), 3)
        self.assertEqual(e.meth2(), 3)
        try:
            d.a
        except AttributeError:
            pass
        else:
            self.fail("attribute should have vanished")

        try:
            D.__bases__ = (Classic,)
        except TypeError:
            pass
        else:
            self.fail("new-style class must have a new-style base")

    def test_mutable_bases_catch_mro_conflict(self):
        # Testing mutable bases catch mro conflict...
        class A(object):
            pass

        class B(object):
            pass

        class C(A, B):
            pass

        class D(A, B):
            pass

        class E(C, D):
            pass

        try:
            C.__bases__ = (B, A)
        except TypeError:
            pass
        else:
            self.fail("didn't catch MRO conflict")

tests = CPythonTests()
tests.test_mutable_bases()
tests.test_mutable_bases_catch_mro_conflict()
print
print "Finished"
