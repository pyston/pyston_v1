import weakref
import gc

class C(object):
    def foo(self):
        print "inside foo()"

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

def getWR():
    c = C()
    wr = weakref.proxy(c)
    wr.attr = "test attr"
    print wr.attr, c.attr
    wr.foo()
    del c
    return wr

wr = getWR()
fact(100) # try to clear some memory
gc.collect()

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
