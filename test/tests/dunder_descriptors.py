# - descriptors

#  Descriptors get processed when fetched as part of a dunder lookup

def f1():
    class D(object):
        def __init__(self, n):
            self.n = n
        def __get__(self, obj, cls):
            print "__get__()", obj is None, self.n
            def desc(*args):
                print "desc()", len(args)
                return self.n
            return desc

        def __call__(self):
            print "D.call"
            return self.n

    class C(object):
        __hash__ = D(1)
        __add__ = D(2)
        __init__ = D(None)

    print C.__init__()
    c = C()
    print C.__hash__()
    print c.__hash__()
    print hash(c)
    print c + c
f1()

def f2():
    print "\nf2"
    class D(object):
        def __call__(self, subcl):
            print "call", subcl
            return object.__new__(subcl)

    def get(self, inst, owner):
        print "__get__", inst, owner
        def new(self):
            print "new"
            return object.__new__(owner)
        return new

    class C(object):
        __new__ = D()

    print type(C())
    D.__get__ = get
    print type(C())
f2()

def f3():
    print "\nf3"
    class D(object):
        def __call__(self):
            print "call"
            return None

    def get(self, inst, owner):
        print "__get__", type(inst), owner
        def init():
            print "init"
            return None
        return init

    class C(object):
        __init__ = D()

    print type(C())
    D.__get__ = get
    print type(C())
f3()

# misc tests:
import sys
sys.getrecursionlimit.__call__.__call__.__call__()
TypeError.__call__.__call__.__call__()
