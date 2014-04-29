# expected: fail
# - this particular check isn't implemented yet

class M(type):
    def __instancecheck__(self, rhs):
        print "M.instancecheck",
        return True
    def __subclasscheck__(self, rhs):
        print "M.subclasscheck",
        return True

class A(object):
    __metaclass__ = M
    def __add__(self, rhs):
        print "A.add()"
    def __radd__(self, rhs):
        print "A.radd()"

class B(A):
    __metaclass__ = type
    def __add__(self, rhs):
        print "B.add()"
    def __radd__(self, rhs):
        print "B.radd()"

class C(object):
    def __add__(self, rhs):
        print "C.add()"
    def __radd__(self, rhs):
        print "C.radd()"

def add(x, y):
    print type(x), type(y), isinstance(y, type(x)), issubclass(type(y), type(x))
    x + y

add(A(), A())
add(A(), B())
add(A(), C())
add(B(), B())
add(B(), C())
