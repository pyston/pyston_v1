class C(object):
    def __delattr__(self, attr):
        print "__delattr__"
        del C.__delattr__
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
c = C()
c.a = 1
try:
    c.a = 2
except Exception as e:
    print e
