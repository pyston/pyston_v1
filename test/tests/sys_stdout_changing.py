# expected: fail

# prints without an explicit destination should go to sys.stdout, not to the real stdout,
# in case sys.stdout gets changed:

import sys

class StringBuf(object):
    def __init__(self):
        self.s = ""

    def write(self, s):
        self.s += s

    def getvalue(self):
        return self.s

sys_stdout = sys.stdout
sys.stdout = StringBuf()

print "hello world"

print >>sys_stdout, "stringio contains:", repr(sys.stdout.getvalue())

