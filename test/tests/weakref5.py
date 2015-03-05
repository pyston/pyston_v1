# expected: fail
# we aren't collecting everything we should, so wr_cleared isn't being called

import weakref
import gc

class Foo:
    def wr_cleared(self, wr):
        print "made it here!", wr
    def meth(self):
        1
    def doSomething(self):
        bound_meth = self.meth
        self.__wr = weakref.ref(bound_meth, self.wr_cleared)
        return bound_meth

f = Foo()
b = f.doSomething()
gc.collect()
print "before setting f to None"
f = None
gc.collect()
print "after setting f to None"
b = None
gc.collect()
print "after setting b to None"
