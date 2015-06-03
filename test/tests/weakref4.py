import weakref

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth)
  return wr

def recurse(f, n):
    if n:
        return recurse(f, n-1)
    return f()


w = recurse(doStuff, 100)

# Try creating a large object to make sure we can handle them:
def f():
    class C(object):
        # Adding a __slots__ directive increases the size of the type object:
        __slots__ = ['a' + str(i) for i in xrange(1000)]
    return weakref.ref(C)


r = recurse(f, 100)

import gc
gc.collect()
assert r() is None, "object was not collected"
assert w() is None, "object was not collected"
