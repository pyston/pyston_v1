class C():
    pass

print C, type(C)
print map(str, C.__bases__), C.__name__
print type(C())

try:
    C(1)
except TypeError, e:
    print e

class D(C):
    pass

print D, type(D)
print map(str, D.__bases__), D.__name__
print type(D())

ClassType = type(C)

try:
    ClassType.__new__(int, 1, 1, 1)
except TypeError, e:
    print e

try:
    ClassType.__new__(1, 1, 1, 1)
except TypeError, e:
    print e

print ClassType("test", (), {})
print ClassType("test", (), {"__module__":"fake"})

class E():
    def __init__(self, n):
        self.n = n

    def foo(self):
        print self.n

    def __str__(self):
        return "E(%d)" % self.n

    def __getitem__(self, x):
        print "getitem"
        return x

    def __len__(self):
        print "len"
        return self.n

    def __call__(self):
        def f(a):
            print "f", a
        return f

    def __eq__(self, other):
        print "eq"
        return self.n == other.n

    def __ne__(self, other):
        print "ne"
        return self.n != other.n

e = E(1)
print e
print e.n
print e.foo()
print e[1]
print e[1:2]
print len(e)
print e()("test")
print e == E(1)
print e != E(1)

def str2():
    return "str2"
e.__str__ = str2
print e

print bool(e)
print bool(E(0))
print bool(E)

class F:
    def __init__(self, n):
        self.n = n

    def __nonzero__(self):
        return self.n

print bool(F(0))
print bool(F(1))
print F(0) == F(0)
print F(0) != F(0)

f = F(0)
try:
    len(f)
except AttributeError, e:
    print e

try:
    f[1]
except AttributeError, e:
    print e

try:
    f[1] = 2
except AttributeError, e:
    print e

print isinstance(f, F)
print isinstance(e, F)
print isinstance(D(), D)
print isinstance(D(), C)

print str(f)[:26]
print repr(f)[:26]


class OldStyleClass:
    pass

print issubclass(OldStyleClass, object)

print isinstance(OldStyleClass(), OldStyleClass)
print issubclass(OldStyleClass, OldStyleClass)

class GetattrTest:
    def __getattr__(self, attr):
        print "getattr", attr
        if attr.startswith("__"):
            raise AttributeError(attr)
        return 1

g = GetattrTest()
g.b = 2
print g.a
print g.b
print g.__class__
print g.__dict__.items()
print bool(g)

class SetattrTest:
    def __setattr__(self, attr, value):
        print "setattr", attr, value

    def __delattr__(self, attr):
        print "delattr", attr

s = SetattrTest()
s.b = 2
print s.__dict__.items()
del s.b

class MappingTest:
    def __getitem__(self, key):
        print "getitem", key
        return 1
    def __setitem__(self, key, value):
        print "setitem", key, value
    def __delitem__(self, key):
        print "delitem", key

m = MappingTest()
m[1] = 2
del m[2]
print m[3]

class Hashable:
    def __hash__(self):
        return 5
print hash(Hashable())
del Hashable.__hash__
print type(hash(Hashable()))

class C():
    def foo(self):
        pass

    @classmethod
    def bar(cls):
        print cls

c = C()
print type(C.foo)
print type(getattr(C, "foo"))
print type(getattr(C, "foo").im_func)
c.bar()
C.bar()
try:
    C.doesnt_exist
except AttributeError as e:
    print e


class C():
    pass
print type(C) # classobj

class D(C):
    pass
print type(D) # classobj

# Inheriting from old + new style classes gives a new-style class
class E(C, object):
    pass
print type(E) # type
class F(object, C):
    pass
print type(F) # type

print type("aoeu", (str, object), {})
# ClassType has to defer to calling type(b) for the first non-oldstyle base
print type(ClassType("aoeu", (str, object), {}))

# Even if that is not a new-style class!
class MyCustomClass(object):
    def __init__(self, *args, **kw):
        print "init", args[:-1], kw

    def __repr__(self):
        return "<MCC>"

# type(MyCustomClass()) is MyCustomClass, which is callable, leading to another call to __init__
print ClassType("aoeu", (MyCustomClass(), ), {})

class D():
    def test(self):
        return "D.test"

class LateSubclassing():
    def __init__(self):
        LateSubclassing.__bases__ = (C, D)
print LateSubclassing().test()
print issubclass(LateSubclassing, C)
print issubclass(LateSubclassing, D)
print issubclass(LateSubclassing, E)

# Mixed old and new style class inheritance
class C():
    pass
class D(C):
    pass
class E(C, object):
    pass
print issubclass(D, C), isinstance(D(), C)
print issubclass(E, C), isinstance(E(), C)
print isinstance(E, object), isinstance(E(), object)

class SeqTest:
    class Iterator:
        def __init__(self):
            self.n = 5
        def next(self):
            print "next"
            if self.n <= 0:
                raise StopIteration()
            r = self.n
            self.n -= 1
            return r
    def __iter__(self):
        print "iter"
        return SeqTest.Iterator()
m = SeqTest()
print list(m)

class OldSeqTest:
    def __getitem__(self, n):
        print "getitem", n
        if n > 5:
            raise IndexError()
        return n ** 2
m = OldSeqTest()
print list(m)

import sys
class E:
    def __init__(self, *args):
        print "__init__", args
    def __repr__(self):
        return "<E object>"
try:
    raise E
except:
    print sys.exc_info()[0].__name__, sys.exc_info()[1]
try:
    raise E, 1
except:
    print sys.exc_info()[0].__name__, sys.exc_info()[1]
try:
    raise E()
except:
    print sys.exc_info()[0].__name__, sys.exc_info()[1]
try:
    raise E(), 1
except:
    print sys.exc_info()[0].__name__, sys.exc_info()[1]

