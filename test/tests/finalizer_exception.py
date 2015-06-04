# expected: fail
# not sure how to redirect the stderr from Pyston using only Python code
# the testing suite currently thinks that a non-empty stderr means fail
import sys
from testing_helpers import test_gc

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

test_gc(test, 10)

print sorted(strs)

print "done"
