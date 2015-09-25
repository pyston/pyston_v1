# Exceptions from finalizers should get caught:
import sys
from testing_helpers import test_gc

class Writer(object):
    def write(self, data):
        print "something printed to stderr"

sys.stderr = Writer()

strs = []

class C(object):
    def __init__(self, index):
        self.index = index

    def __del__(self):
        strs.append("never do this %d" % self.index)
        raise Exception("it's a bad idea")

def test():
    cs = [C(i) for i in range(10)]

test_gc(test, 10)

print sorted(strs)



# Similarly for exceptions from weakref callbacks:
import weakref

called_callback = False
def callback(ref):
    global called_callback
    if not called_callback:
        print "callback"
        called_callback = True
        raise ValueError()

class C(object):
    pass

import gc
l = []

# Make a bunch of them just to make sure at least one gets collected:
for i in xrange(100):
    l.append(weakref.ref(C(), callback))

gc.collect()
