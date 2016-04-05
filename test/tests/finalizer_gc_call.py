import gc
from testing_helpers import test_gc

# This tests the edge case where a garbage collection gets triggered inside
# a finalizer. Finalizers can allocate objects so this can definitely happen
# in practice.

indices = {}

class GCCaller(object):
    def __del__(self):
        gc.collect()

class ObjWithFinalizer(object):
    def __init__(self, index):
        self.index = index
    def __del__(self):
        global indices
        indices[self.index] = True

def scope():
    for _ in xrange(200):
        for i in xrange(20):
            obj = ObjWithFinalizer(i)
        caller = GCCaller()

test_gc(scope)
