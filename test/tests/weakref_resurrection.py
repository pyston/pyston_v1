# expected: fail
# It's hard to guarantee the order of weakref callbacks being called
# when we have a GC
import weakref
import gc

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

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

wr1, wr2 = foo1()
wr3, wr4 = foo2()

# make sure to override remaining references to the weakref
# in the stack since the GC will scan the stack conservatively
fact(10)

gc.collect()
gc.collect()
gc.collect()
gc.collect()
gc.collect()

print wr1(), wr2()
print wr3(), wr4()
