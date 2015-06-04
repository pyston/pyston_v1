# expected: fail
# It's hard to guarantee the order of weakref callbacks being called
# when we have a GC
import weakref
from testing_helpers import test_gc

def callback(wr):
    print "object was destroyed", wr()

class C(object):
    def __init__(self, index):
        self.index = index

saved_wrs = []
def weak_retainer(to_be_resurrected):
    def cb(wr):
        global saved_wr
        saved_wrs.append(to_be_resurrected())
        print "staying alive~", wr, to_be_resurrected
    return cb

def foo1():
    c1 = C(1)
    c2 = C(2)
    wr1 = weakref.ref(c1, callback)
    wr2 = weakref.ref(c2, weak_retainer(wr1))
    return (wr1, wr2)

def foo2():
    c3 = C(3)
    c4 = C(4)
    wr4 = weakref.ref(c4, callback)
    wr3 = weakref.ref(c3, weak_retainer(wr4))
    return (wr3, wr4)

wr1, wr2 = test_gc(foo1, 5)
wr3, wr4 = test_gc(foo2, 5)

print wr1(), wr2()
print wr3(), wr4()
