# Simple test:
class MyClass(object):
    __a = 1
    print sorted(locals().items()) # This should contain the mangled name

print hasattr(MyClass, "__a")
print hasattr(MyClass, "_MyClass__a")

# Names in functions get mangled:
class MyClass(object):
    def __init__(self):
        # attributes get mangled:
        self.__x = 1

    def f(self):
        print __g
        __local = 1
        print sorted(locals().keys()) # This should contain the mangled name
        print __k

        del __k # this should delete the mangled name
        print sorted(locals().keys())

        print self.__x

__g = 5
_MyClass__g = 6
try:
    MyClass().f()
except NameError, e:
    print e
print MyClass()._MyClass__x


# This includes function arguments!
class MyClass(object):
    def f(self, __x):
        print __x

    def g(self, *__args, **__kw):
        print __args, __kw

MyClass().f(5)
try:
    MyClass().f(__x=5)
except TypeError, e:
    print e
MyClass().f(_MyClass__x=6)
MyClass().g(5, a=5)

# Function names get mangled
class MyClass(object):
    def __f(self):
        pass
# But they still keep their old name for display:
print MyClass._MyClass__f.im_func.__name__

# And classdefs get mangled too I guess:
class MyClass(object):
    class __InnerClass(object):
        pass

# And imports do too??
class MyClass(object):
    try:
        import __foo
    except ImportError, e:
        print e

class MyClass(object):
    try:
        import __foo as __bar
    except ImportError, e:
        print e

class MyClass(object):
    import sys as __bar
    print __bar

class MyClass(object):
    try:
        from __foo import __bar
    except ImportError, e:
        print e

#TODO enable this once we support `import *` in functions
#class MyClass(object):
#    try:
#        from __foo import *
#    except ImportError, e:
#        print e

class MyClass(object):
    try:
        from sys import __bar
    except ImportError, e:
        print e

class MyClass(object):
    try:
        # Except if it's a dotted name:
        import __foo.__bar
    except ImportError, e:
        print e.message

# names inside classes with mangled names don't get the mangled class name:
class MyClass(object):
    class __InnerClass(object):
        __inner_inner = "hi"
        print sorted(locals().items())
print MyClass._MyClass__InnerClass._InnerClass__inner_inner

# This class gets accessed through the mangled name, but its stored name is still the original name.
print MyClass._MyClass__InnerClass

class MyClass(object):
    def f(self):
        self.__x = 1
        def inner():
            # Names inner functions also get mangled:
            return self.__x
        return inner
print MyClass().f()()

# Eval not supported:
"""
class MyClass(object):
    def f(self):
        # Sanity check:
        y = 2
        print eval("y")

        __x = 1

        try:
            # Names do not get mangled inside of an eval:
            print eval("__x")
        except NameError, e:
            print e
MyClass().f()
"""

# Things get mangled in different types of sub-scopes: lambdas, generator expressions, generators:
class MyClass(object):
    def f(self):
        __x = 2
        def g():
            yield __x

        print list(g())
        print list((__k * __x for __k in xrange(5)))
        print map(lambda x: x ** __x, range(5))
        print [x - __x for x in xrange(4)]
MyClass().f()

_MyClass__x = 0
class MyClass(object):
    def f(self):
        global __x
        __x = 1
MyClass().f()
print "_MyClass__x:", _MyClass__x


# Random tests from looking at the behavior of _Py_Mangle:

# Leading underscores on the class name get stripped:
class _MyClass(object):
    __a = 3
print _MyClass._MyClass__a

# But if the class is all underscores, it doesn't mangle anything
# (weird corner case but makes sense, since that new name would be hard
# to reference)
class ___(object):
    __a = 2
print ___.__a

# this fails if the type analysis does not mangle the function name
class C():
    def f():
        def __f2():
            print "hi"
        if 1:
            pass
    f()

