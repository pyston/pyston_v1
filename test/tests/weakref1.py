import weakref
import gc

def cb(wr):
  print "object was destroyed", wr()

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth, cb)
  return wr

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)


w = doStuff()
fact(10) # try to clear some memory
gc.collect()
