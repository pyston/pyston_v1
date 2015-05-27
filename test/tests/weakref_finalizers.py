# test to ensure that weakref callbacks and finalizers get called in the
# right order

import weakref
import gc

def callback(wr):
    print "object was destroyed", wr()

def retainer(ref):
    def cb(wr):
        print "object was destroyed", ref, wr()
    return cb

class C(object):
    def __init__(self, index):
        self.index = index
    def __del__(self):
        print "deleted", self.index

def strong_scope():
    # These go out of scope first.
    c1 = C(1)
    c2 = C(2)
    c3 = C(3)

    normal_weakref1 = weakref.ref(c1, callback)
    normal_weakref2_1 = weakref.ref(c2, callback)
    normal_weakref2_2 = weakref.ref(c2, callback)
    adverserial_weakref = weakref.ref(c3, retainer(c3))
    return (normal_weakref1, normal_weakref2_1, normal_weakref2_2)


def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

weakrefs = strong_scope()

# make sure to override remaining references to the objects
# in the stack since the GC will scan the stack conservatively
fact(10)

gc.collect()
gc.collect()
