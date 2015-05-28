# expected: fail
# not sure how to redirect the stderr from Pyston using only Python code
# the testing suite currently thinks that a non-empty stderr means fail
import gc
import sys

class Writer(object):
    def write(self, data):
        pass
        #print "something printed to stderr"

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

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

test()

# make sure to override remaining references in the
# stack since the GC will scan the stack conservatively
fact(10)

gc.collect()
gc.collect()
gc.collect()
gc.collect()
gc.collect()
gc.collect()
gc.collect()
gc.collect()

print sorted(strs)

print "done"
