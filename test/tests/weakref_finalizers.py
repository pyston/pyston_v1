# test to ensure that weakref callbacks and finalizers get called in the
# right order

import weakref
from testing_helpers import test_gc

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

def scope_old1():
    c1 = OldStyle(1)
    return weakref.ref(c1, callback)

def scope_old2():
    c2 = OldStyle(2)
    return (weakref.ref(c2, callback), weakref.ref(c2, callback))

def scope_old3():
    c3 = OldStyle(3)
    adverserial_weakref = weakref.ref(c3, retainer(c3))

def scope_new1():
    c1 = NewStyle(1)
    return weakref.ref(c1, callback)

def scope_new2():
    c2 = NewStyle(2)
    return (weakref.ref(c2, callback), weakref.ref(c2, callback))

def scope_new3():
    c3 = NewStyle(3)
    adverserial_weakref = weakref.ref(c3, retainer(c3))

print ">> Test old style"
test_gc(scope_old1)
test_gc(scope_old2)
test_gc(scope_old3, 3)

print ">> Test new style"
test_gc(scope_new1)
test_gc(scope_new2)
test_gc(scope_new3, 3)
