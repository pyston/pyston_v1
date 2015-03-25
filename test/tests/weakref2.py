# expected: fail
# we aren't collecting everything we should, so wr_cleared isn't being called

import weakref
import gc

class Foo:
    def wr_cleared(self, wr):
        print "made it here!", wr
    def meth(self):
         1
    def createCycle(self):
         self.__bound_meth = self.meth
         self.__wr = weakref.ref(self.__bound_meth, self.wr_cleared)
         return self.__bound_meth

def bwr_cleared(wr):
    print "made it here for b!", wr

f = Foo()
b = f.createCycle()
bwr = weakref.ref(b, bwr_cleared)
gc.collect()
print bwr
print "before setting f to None"
f = None
gc.collect()
print bwr
print "after setting f to None"
b = None
gc.collect()
print bwr
print "after setting b to None"
