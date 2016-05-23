class C(object):
    pass

def f():
    c = C()
    c.a = 1
    del c.a

f()
def __delattr__(self, attr):
    print "custom __delattr__!"
C.__delattr__ = __delattr__
f()
del C.__delattr__
f()
