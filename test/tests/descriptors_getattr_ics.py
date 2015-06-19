# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 80

class DataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        return 0
    def __set__(self, obj, value):
        pass

class NonDataDescriptor(object):
    def __get__(self, obj, typ):
        print '__get__ called'
        print type(self)
        print type(obj)
        print typ
        return 1

class C(object):
    a = DataDescriptor()
    b = NonDataDescriptor()
    c = NonDataDescriptor()

inst = C()

inst.c = 100
inst.d = 101
C.d = DataDescriptor()

def f():
    print inst.a
    print inst.b
    print inst.c
    print inst.d

    print C.a
    print C.b

for i in xrange(1000):
    f()
