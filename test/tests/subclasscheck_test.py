class M(type):
    def __instancecheck__(self, rhs):
        print "M.instancecheck",
        return True
    def __subclasscheck__(self, rhs):
        print "M.subclasscheck",
        return True

class A(object):
    __metaclass__ = M

class B(A):
    __metaclass__ = type

print type(A), isinstance(A(), B), issubclass(A, B)
print type(B), isinstance(B(), B), issubclass(B, B)
print type(int), isinstance(int(), B), issubclass(int, B)
