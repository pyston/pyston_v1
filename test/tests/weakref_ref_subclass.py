# Make sure we can subclass from weakref.ref, since the weakref module itself does this

from weakref import ref
class MyRef(ref):
    pass

for i in xrange(100):
    m = MyRef(MyRef)

import gc
gc.collect()
