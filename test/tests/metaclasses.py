class MM(type):
    def __new__(*args):
        print "MM.__new__", args[:3]
        return type.__new__(*args)

    def __call__(*args):
        print "MM.__call__", args[:3]
        return type.__call__(*args)

print "Made MM", type(MM)

class M(type):
    __metaclass__ = MM

    def __new__(*args):
        print "M.__new__", args[:3]
        return type.__new__(*args)

    def __call__(*args):
        print "M.__call__", args[:3]
        return type.__call__(*args)

print "Made M", type(M)

class C(object):
    __metaclass__ = M

print "Made C", type(C)
print isinstance(C, M)
print isinstance(C, type)
print isinstance(C, int)

def f(*args):
    print "f()", args[:2]
class C(object):
    # Metaclasses don't need to be type objects:
    __metaclass__ = f
print C

print type.__call__(int, 1)

try:
    type.__new__(1, 2, 3)
except TypeError, e:
    print e

try:
    type.__new__(int, 1, 2, 3)
except TypeError, e:
    print e


class D():
    __metaclass__ = type
print D
print D.__base__

print type("test", (), {})
print type("test", (), {"__module__":"fake"})

# test non str attr keys
t = type("test", (), {u"test" : 2, 1000L : 3, 1.0 : 4})
print t.__dict__[u"test"], t.test
print t.__dict__[1000L]
print t.__dict__[1.0]
