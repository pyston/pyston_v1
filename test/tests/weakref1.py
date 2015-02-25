import weakref
import gc

def cb(wr):
  print "object was destroyed", wr()

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth, cb)
  return wr


w = doStuff()
gc.collect()
