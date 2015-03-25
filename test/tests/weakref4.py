import weakref
import gc

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth)
  return wr


w = doStuff()
gc.collect()
print w()
