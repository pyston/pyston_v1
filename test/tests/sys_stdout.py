# expected: fail

import sys

sys.stdout.write("hello world\n")
print >>sys.stdout, "hello world"
