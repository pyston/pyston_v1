# run_args: -n
# statcheck: stats['slowpath_callattr'] <= 80
# statcheck: stats['slowpath_getattr'] <= 80

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
