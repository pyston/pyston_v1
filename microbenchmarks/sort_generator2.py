import itertools
import sys

N = 5
if len(sys.argv) > 1:
    N = int(sys.argv[1])

print "def f(l):"
for i in xrange(N):
    for j in xrange(i + 1, N):
        print "  if l[%d] > l[%d]: l[%d], l[%d] = l[%d], l[%d]" % (i, j, i, j, j, i)
print "  return l"

import random
for i in xrange(10):
    l = range(N)
    random.shuffle(l)
    print "print f(%s)" % (l,)
