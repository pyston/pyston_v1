# expected: fail

import sys

old_stdout = sys.stdout

class SoftspaceTest(object):
    def write(self, str):
        print >>old_stdout, self.softspace
        old_stdout.write(str)

sys.stdout = SoftspaceTest()

print "hello"
print "world"

print "hello", "world"

print "hello",
print "world",
