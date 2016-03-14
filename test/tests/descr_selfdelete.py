import sys

class C(object):
    def __delattr__(self, attr):
        print "__delattr__"
        del C.__delattr__
        sys._clear_type_cache()
c = C()
del c.a
try:
    del c.a
except Exception as e:
    print e

class C(object):
    def __getattr__(self, attr):
        print "__getattr__"
        del C.__getattr__
        sys._clear_type_cache()
        return attr
c = C()
print c.a
try:
    print c.a
except Exception as e:
    print e

class C(object):
    def __setattr__(self, attr, val):
        print "__setattr__", attr, val
        del C.__setattr__
        sys._clear_type_cache()
c = C()
c.a = 1
try:
    c.a = 2
except Exception as e:
    print e


class D(object):
    def __get__(self, obj, type):
        print "D.__get__"
        del D.__get__
        sys._clear_type_cache()
        return 1

    def __set__(self, obj, value):
        print "D.__set__"
        del D.__set__
        sys._clear_type_cache()

C.x = D()
c = C()
c.x = 0
print c.x
c.x = 0
print c.x

