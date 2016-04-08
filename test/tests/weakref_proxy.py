import weakref
from testing_helpers import test_gc

class C(object):
    def foo(self):
        print "inside foo()"

def getWR():
    c = C()
    wr = weakref.proxy(c)
    wr.attr = "test attr"
    print wr.attr, c.attr
    wr.foo()
    del c
    return wr

wr = test_gc(getWR)

try:
    wr.foo()
except ReferenceError as e:
    print e

try:
    print wr.attr
except ReferenceError as e:
    print e

try:
    wr.attr = "val2"
except ReferenceError as e:
    print e
