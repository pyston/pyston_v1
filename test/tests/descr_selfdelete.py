# Test some internals: when we execute some special functions, we often need to
# make sure to keep a reference to that function, in case that function ends up
# unsetting itself.
#
# A couple notes about this test:
# - I think the _clear_type_cache calls are unnecessary, since the type cache is all borrowed references,
#   but I left them in just in case.
# - Pyston is relatively capable of continuing to run a function that has been deallocated,
#   since most of the metadata is stored on another structure that has a longer lifetime.
#   So to try to get around that, this test adds some "other_arg" default arguments, since those
#   will get collected when the function gets destroyed.
#   Actually, I guess it can run ok since the defaults will get incref'd before the function gets called.
#   So also test it by creating a closure and make sure that that stays alive.
# - This file wraps each individual test in a function and then calls it twice -- this is to try to test
#   any rewritten (traced) variants as well.

import sys

def make_closure(f):
    def inner(*args, **kw):
        print f.__name__
        r = f(*args, **kw)
        print f.__name__
        return r
    return inner

# The standard case, when the function is obtained via standard evaluation rules:
class C(object):
    @make_closure
    def f(self, other_arg=2.5**10):
        print "f"
        print other_arg
        del C.f
        print other_arg
c = C()
c.f()

def f():
    class C(object):
        @make_closure
        def __delattr__(self, attr, other_arg=2.3**10):
            print "__delattr__"
            print other_arg
            del C.__delattr__
            print other_arg
            sys._clear_type_cache()
            print other_arg
    c = C()
    del c.a
    try:
        del c.a
    except Exception as e:
        print e
f()
f()

def f():
    class C(object):
        @make_closure
        def __getattr__(self, attr, other_arg=2.2**10):
            print "__getattr__"
            print other_arg
            del C.__getattr__
            print other_arg
            sys._clear_type_cache()
            print other_arg
            return attr
    c = C()
    print c.a
    try:
        print c.a
    except Exception as e:
        print e
f()
f()

def f():
    class C(object):
        @make_closure
        def __setattr__(self, attr, val, other_arg=2.1**10):
            print "__setattr__", attr, val
            print other_arg
            del C.__setattr__
            print other_arg
            sys._clear_type_cache()
            print other_arg
    c = C()
    c.a = 1
    try:
        c.a = 2
    except Exception as e:
        print e
f()
f()


def f():
    class D(object):
        @make_closure
        def __get__(self, obj, type, other_arg=2.4**10):
            print "D.__get__"
            print other_arg
            del D.__get__
            print other_arg
            sys._clear_type_cache()
            print other_arg
            return 1

        @make_closure
        def __set__(self, obj, value, other_arg=2.0**10):
            print "D.__set__", obj, value
            print other_arg
            del D.__set__
            print other_arg
            sys._clear_type_cache()
            print other_arg
            return 1


    C.x = D()
    c = C()
    c.x = 0
    print c.x
    c.x = 0
    print c.x
f()
f()

# TODO: lots of other cases that could/should get tested.


# Related:
@make_closure
def f(a1=2.0**10, a2=2.1**10):
    f.func_defaults = ()
    print a1, a2
    del globals()['f']
f()
