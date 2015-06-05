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


l1 = []
for i in xrange(5):
    w = recurse(doStuff, 100)
    l1.append(w)

# Try creating a large object to make sure we can handle them:
def f():
    class C(object):
        # Adding a __slots__ directive increases the size of the type object:
        __slots__ = ['a' + str(i) for i in xrange(1000)]
    return weakref.ref(C)

l2 = []
for i in xrange(5):
    r = recurse(f, 100)
    l2.append(r)

import gc
gc.collect()
assert any(r() is None for r in l2), "object was not collected"
assert any(w() is None for w in l1), "object was not collected"
