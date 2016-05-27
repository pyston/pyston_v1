# expected: fail
# this only works in the interpreter and not in the bjit and llvm jit

class C(object):
    def next(self):
        return 1
    def __del__(self):
        print "del"
class D(object):
    def __iter__(self):
        return C()

def f():
    for i in D():
        print i
        break
for i in xrange(100):
    print
    f()
