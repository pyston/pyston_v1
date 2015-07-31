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

    def __neg__(self):
        print "neg"
        return -self.n

    def __pos__(self):
        print "pos"
        return +self.n

    def __abs__(self):
        print "abs"
        return abs(self.n)

    def __invert__(self):
        print "invert"
        return ~self.n

    def __int__(self):
        print "int"
        return int(self.n)

    def __long__(self):
        print "long"
        return long(self.n)

    def __float__(self):
        print "float"
        return float(self.n)

    def __oct__(self):
        print "oct"
        return oct(self.n)

    def __hex__(self):
        print "hex"
        return hex(self.n)

    def __coerce__(self, other):
        print "coerce"
        return (int(self.n), other)

    def __index__(self):
        print "index"
        return self.n

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
print -e
print +e
print abs(e)
print ~e
print int(e)
print long(e)
print float(e)
print oct(e)
print hex(e)
print coerce(e, 10)
test_list = ["abc", "efg", "hij"]
print test_list[e]

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


class G:
    def __init__(self, n):
        self._n = n

    def __pow__(self, other, modulo=None):
        print "pow in instance"

    def __rpow__(self, other):
        print "rpow in instance"

    def __ipow__(self, other, modulo=None):
        print "ipow in instance"

p1 = G(3)
p2 = G(4)

p1 ** p2
4 ** p2
None ** p2
p2 ** None

pow(p1, p2)
pow(p1, None)
pow(None, p1)

pow(p1, p2, p2)
pow(p1, p2, None)

p1.__ipow__(p1, p2)
p1 **= p2


class H:
    def __init__(self, n):
        self._n = n

p3 = H(3)
p4 = H(4)
p5 = G(5)

pow(p5, p3)
pow(p3, p5)

try:
    pow(p3, p4)
except Exception as e:
    print type(e), e.message

try:
    pow(p3, p4, p4)
except Exception as e:
    assert type(e) in (AttributeError, TypeError)


# mixed calling
class I(object):
    def __init__(self, n):
        self._n = n

    def __pow__(self, other, modulo=None):
        print "pow in object"

    def __rpow__(self, n):
        print "rpow in object"

    def __ipow__(self, other, modulo=None):
        print "ipow in object"

p5 = I(3)

try:
    pow(p3, p5)
except Exception as e:
    print type(e), e.message

try:
    p3 ** p5
except Exception as e:
    print type(e), e.message


class LessEqualTest:
    def __init__(self, a):
        self._a = a

    def __lt__(self, other):
        print "lt"
        return self._a < other._a

    def __le__(self, other):
        print "le"
        return self._a <= other._a

ca = LessEqualTest(3)
cb = LessEqualTest(2)
cc = LessEqualTest(2)

print(ca < cb)
print(cb < ca)

print(ca <= cb)
print(ca <= cc)

# CompareA only defines __lt__ and __le__ but appears on the right-hand-side
print(ca > cb)
print(ca >= cb)
print(cb > ca)
print(cb > cc)

class GreatEqualTest:
    def __init__(self, a):
        self._a = a

    def __gt__(self, other):
        print "gt"
        return self._a > other._a

    def __ge__(self, other):
        print "ge"
        return self._a >= other._a

cd = GreatEqualTest(4)
ce = GreatEqualTest(5)
cf = GreatEqualTest(4)

print(cd >= ce)
print(cd > ce)
print(cd >= cf)
print(cd > cf)

# CompareB only defines __gt__ and __ge__ but appears on the right-hand-side
print(cd <= ce)
print(cd < ce)
print(cd <= cf)
print(cd < cf)

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

