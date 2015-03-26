import weakref
import gc

def doStuff():
  def meth():
    pass

  wr = weakref.ref(meth)
  return wr

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)


w = doStuff()
print fact(10) # try to clear some memory
gc.collect()
print w()
