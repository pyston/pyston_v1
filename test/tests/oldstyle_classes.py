# __bases__ and __name__ not supported yet -- need to add a custom getattr() method for old style classes

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

e = E(1)
print e
print e.n
print e.foo()
print e[1]
print e[1:2]
print len(e)

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
