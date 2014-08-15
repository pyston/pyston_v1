print "starting import of", __name__

import import_nested_target

x = 1

def foo():
    print "foo()"

# def k():
    # print x
    # foo()

class C(object):
    pass

_x = 1
__all__ = ['x']

def letMeCallThatForYou(f, *args):
    return f(*args)
