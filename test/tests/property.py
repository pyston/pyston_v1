# I think pypa has an issue parsing decorator expressions if they aren't simple names
# https://github.com/vinzenz/libpypa/issues/15

class C(object):
    def fget(self):
        return 5

    def fset(self, val):
        print 'in fset, val =', val

    x = property(fget, fset, None, "Doc String")

c = C()
print c.x
print C.x.__get__(c, C)
print type(C.x.__get__(None, C))
c.x = 7
print c.x
print C.x.__doc__

class C2(object):
    @property
    def x(self):
        print "x1"
        return 2

    x1 = x

    @x.setter
    def x(self, value):
        print "x2"
        return 3

    x2 = x

    @x.deleter
    def x(self):
        print "x3"

c = C2()

print "These should all succeed:"
print c.x1
print c.x2
print c.x

try:
    # This will fail since x1 is a copy that didn't have the setter set:
    c.x1 = 1
except AttributeError, e:
    print e
c.x2 = 1
c.x = 1


try:
    # This will fail since x1 is a copy that didn't have the deleter set:
    del c.x1
except AttributeError, e:
    print e
try:
    # This will fail since x1 is a copy that didn't have the deleter set:
    del c.x2
except AttributeError, e:
    print e
c.x = 1

class MyProperty(property):
    pass

class C(object):
    v = "empty"
    @MyProperty
    def p(self):
        print "get"
        return self.v

    @p.setter
    def p(self, value):
        print "set"
        self.v = "it " + value

c = C()
c.p = "worked"
print c.p

print 'test the setting of __doc__'
class C(object):
    @property
    def f(self):
        """doc string of f"""
print C.f.__doc__

print 'test the setting of __doc__ with a __get__'
class Desc(object):
    def __get__(self, obj, typ):
        print 'desc called'
        return "blah"
class ObjWithDocDesc(object):
    __doc__ = Desc()
class C(object):
    f = property(ObjWithDocDesc)
print C.f.__doc__

print 'test the setting of __doc__ with a __get__ throwing an exception (should get swallowed)'
class Desc(object):
    def __get__(self, obj, typ):
        raise ValueError("arbitrary exception")
class ObjWithDocDesc(object):
    __doc__ = Desc()
class C(object):
    f = property(ObjWithDocDesc)
print C.f.__doc__

print 'test the setting of __doc__ with a __get__ throwing an exception (should not get swallowed)'
class Desc(object):
    def __get__(self, obj, typ):
        raise BaseException("not a subclass of Exception")
class ObjWithDocDesc(object):
    __doc__ = Desc()
try:
    class C(object):
        f = property(ObjWithDocDesc)
except BaseException as e:
    print e.message

print 'test the setting of a __doc__ when you copy it'
class Desc(object):
    def __get__(self, obj, typ):
        print 'desc called'
        return "blah"
class ObjWithDocDesc(object):
    __doc__ = Desc()
prop = property(ObjWithDocDesc)
print 'made prop'
print prop.__doc__
def g():
    """doc of g"""
    return 5
prop2 = prop.getter(g)
print 'made prop2'
print prop2.__doc__
prop3 = prop.setter(lambda self, val : None)
print prop3.__doc__
prop4 = prop.deleter(lambda self, val : None)
print prop4.__doc__

print 'test the setting of a __doc__ when you copy it when using a subclass of property'
class PropertySubclass(property):
    pass
class Desc(object):
    def __get__(self, obj, typ):
        print 'desc called'
        return "blah"
class ObjWithDocDesc(object):
    __doc__ = Desc()
prop = PropertySubclass(ObjWithDocDesc)
print 'made prop'
print prop.__doc__
def g():
    """doc of g"""
    return 5
prop2 = prop.getter(g)
print 'made prop2'
print prop2.__doc__
prop3 = prop.setter(lambda self, val : None)
print prop3.__doc__
prop4 = prop.deleter(lambda self, val : None)
print prop4.__doc__
