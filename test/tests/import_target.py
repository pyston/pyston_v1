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
z = 2
__all__ = ['x', u'z']

def letMeCallThatForYou(f, *args):
    return f(*args)

if __name__ == "__main__":
    import sys
    print "running import_target as main"
    print "argv:", sys.argv
