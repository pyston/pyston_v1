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
