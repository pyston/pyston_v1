# expected: statfail
# run_args: -n
# statcheck: stats['slowpath_callattr'] <= 80
# statcheck: stats['slowpath_getattr'] <= 80

# Right now this won't work because callattr involves two calls
# one call to __get__ and then another call to the returned function.
# Of course, if the callattr were split up into getattr and a call,
# each could be re-written separately...
# Not sure if this is a case worth handling or what is the best way
# to handle it, but I'm throwing the test in here anyway to remind us.

def g():
    print 'in g'
    return 0

def h():
    print 'in h'
    return 1

class DataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        return g
    def __set__(self, obj, value):
        pass

class NonDataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        return g

class C(object):
    a = DataDescriptor()
    b = NonDataDescriptor()
    c = NonDataDescriptor()

inst = C()

inst.c = h
inst.d = h
C.d = DataDescriptor()

def f():
    print inst.a()
    print inst.b()
    print inst.c()
    print inst.d()

    print C.a()
    print C.b()

for i in xrange(1000):
    f()
