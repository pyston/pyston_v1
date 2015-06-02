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

class OldStyle():
    def __init__(self, index):
        self.index = index
    def __del__(self):
        print "deleted", self.index

class NewStyle(object):
    def __init__(self, index):
        self.index = index
    def __del__(self):
        print "deleted", self.index

def strong_scope_old():
    # These go out of scope first.
    c1 = OldStyle(1)
    c2 = OldStyle(2)
    c3 = OldStyle(3)

    normal_weakref1 = weakref.ref(c1, callback)
    normal_weakref2_1 = weakref.ref(c2, callback)
    normal_weakref2_2 = weakref.ref(c2, callback)
    adverserial_weakref = weakref.ref(c3, retainer(c3))
    return (normal_weakref1, normal_weakref2_1, normal_weakref2_2)

def strong_scope_new():
    # These go out of scope first.
    c1 = NewStyle(1)
    c2 = NewStyle(2)
    c3 = NewStyle(3)

    normal_weakref1 = weakref.ref(c1, callback)
    normal_weakref2_1 = weakref.ref(c2, callback)
    normal_weakref2_2 = weakref.ref(c2, callback)
    adverserial_weakref = weakref.ref(c3, retainer(c3))
    return (normal_weakref1, normal_weakref2_1, normal_weakref2_2)


def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

print ">> Test old style"
weakrefs = strong_scope_old()

# make sure to override remaining references to the objects
# in the stack since the GC will scan the stack conservatively
fact(10)

gc.collect()
gc.collect()

print ">> Test new style"
weakrefs = strong_scope_new()

fact(10)

gc.collect()
gc.collect()
