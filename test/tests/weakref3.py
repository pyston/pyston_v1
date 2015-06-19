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
         return self.__wr

f = Foo()
b = f.createCycle()
print b
print "before setting f to None"
f = None
print "after setting f to None"
b = None
print "after setting b to None"
gc.collect()
print "after calling gc.collect"
