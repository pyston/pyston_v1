X = 0
Y = 0
Z = 0
W = 0

def f(val, desc):
    print desc
    return val

def wrapper():
    X = 1
    Y = 1
    Z = 1
    W = 1

    print "starting classdef"
    class C(f(object, "evaluating bases")):
        print __doc__, __module__, __name__ # "None __main__ __main__"

        print "inside classdef"
        global Y
        X = 2
        Y = 2

        if 0:
            Z = 2

        # class scopes have different scoping rules than function scopes (!!):
        # In a function scope, if the name has a store but isn't set on this path,
        # referencing it raises a NameError.
        # In a class scope, the lookup resolves to the global scope.
        print Z, W # "0 1".

        # The defaults for a and b should resolve to the classdef definitions, and the default
        # for c should resolve to the wrapper() definition
        def f(self, a=X, b=Y, c=Z, d=W):
            print a, b, c, d # "2 2 0 1"
            # These references should skip all of the classdef directives,
            # and hit the definitions in the wrapper() function
            print X, Y, Z, W # "1 1 1 1"
    print "done with classdef"

    print hasattr(C, 'X')
    print hasattr(C, 'Y')
    print hasattr(C, 'Z')
    print hasattr(C, 'W')

    return C

wrapper()().f()

print X # "0"
print Y # "2" -- got changed in classdef for C

print
class C2(object):
    print __name__
    __name__ = 1
    print __name__
print C2.__name__

print
class C3(object):
    print __module__
    __module__ = 1
    print __module__
print C3.__module__

"""
# not supported (del)
print
class C4(object):
    print __module__
    del __module__
    try:
        print __module__ # this should throw a NameError
        assert 0
    except NameError:
        pass
print C4.__module__
"""

class C5(object):
    try:
        print not_defined
    except NameError:
        print "threw NameError as expected"

def make_class(add_z):
    class C(object):
        if add_z:
            Z = 1
    return C
print hasattr(make_class(False), "Z")
print hasattr(make_class(True), "Z")

