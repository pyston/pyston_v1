import thread
import _weakref

def f():
    global r
    l = thread._local()
    class C(object):
        pass
    o = C()
    r = _weakref.ref(o)
    l.o = o
    del o
    print type(r())
    del l
f()
print type(r())
