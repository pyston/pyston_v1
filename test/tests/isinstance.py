class M(type):
    pass

class C(object):
    __metaclass__ = M

class D(C):
    pass

c = C()
d = D()
i = 0

print "Testing base case:"
print isinstance(i, C), isinstance(c, C), isinstance(d, C), isinstance(c, int)
print

print "Testing custom metaclass instancecheck:"
def m_instancecheck(self, obj):
    print "m_instancecheck", type(self), type(obj)
    return False
M.__instancecheck__ = m_instancecheck
print isinstance(i, C), isinstance(c, C), isinstance(d, C), isinstance(c, int)
print

print "Testing custom class instancecheck:"
def c_instancecheck(obj):
    print "c_instancecheck", type(obj)
    return False
C.__instancecheck__ = c_instancecheck
print isinstance(i, C), isinstance(c, C), isinstance(d, C), isinstance(c, int)
print

del M.__instancecheck__
del C.__instancecheck__

print "Testing custom class getattribute:"
def c_getattribute(self, attr):
    print "c_getattribute", type(self), attr
C.__getattribute__ = c_getattribute
print isinstance(i, C), isinstance(c, C), isinstance(d, C), isinstance(c, int)
print

print "Testing custom metaclass getattribute:"
def m_getattribute(self, attr):
    print "m_getattribute", type(self), attr
M.__getattribute__ = m_getattribute
print isinstance(i, C), isinstance(c, C), isinstance(d, C), isinstance(c, int)
print

del C.__getattribute__
del M.__getattribute__

print "Testing custom instance __class__"
c2 = C()
c2.__dict__['__class__'] = int
print type(c2), c2.__class__ # should be the same; __class__ is a data decriptor
print isinstance(c2, int), isinstance(c, int)
print

print "Testing custom class __class__:"
class E(object):
    __class__ = int
e = E()
print type(e), e.__class__ # should be different!
print isinstance(e, int), isinstance(e, float), isinstance(e, E), isinstance(E, object)
print

print "Testing custom instance __class__ with custom class __class__:"
# Unlike the non-custom-class version, a custom instance __class__ will now
# take effect since the class __class__ is no longer a data descriptor.
e.__dict__['__class__'] = float
print type(e), e.__class__ # __class__ is now float!
print isinstance(e, int), isinstance(e, float), isinstance(e, E), isinstance(E, object)
print

class M(type):
    pass

class M2(M):
    pass

class C(object):
    __metaclass__ = M2

class D(object):
    pass

# Setting instancecheck on a superclass better update the subclasses:
print "checking superclass instancecheck:"
print isinstance(1, C)
M.__instancecheck__ =  m_instancecheck
print isinstance(1, C)
print

class C(object):
    @property
    def __class__(self):
        print "C.__class__ descriptor"
        raise AttributeError()

    def __getattr__(self, attr):
        print "C.__getattr__", attr
        return int

print "Testing __class__ from __getattr__"
c = C()
print c.__class__
print isinstance(c, int)
print

class C(object):
    def __getattribute__(self, attr):
        print "C.__getattribute__", attr
        raise AttributeError()

    def __getattr__(self, attr):
        print "C.__getattr__", attr
        return int

print "Testing __class__ from __getattr__+__getattribute__"
c = C()
print c.__class__
print isinstance(c, int)
print
