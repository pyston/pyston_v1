import weakref
import gc

num_destroyed = 0
def cb(wr):
    global num_destroyed
    num_destroyed += 1

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth, cb)
  return wr

l = [doStuff() for i in xrange(5)]
gc.collect()
gc.collect()
assert num_destroyed >= 1
