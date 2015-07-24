# This is the same as print.py, but using print as a function
# I guess print() as a function doesn't use the softspace technique.

from __future__ import print_function

def f(o, msg):
    print(msg)
    return o

class C(object):
    def write(self, s):
        print("class write", repr(s))

c = C()
c2 = C()

def write2(s):
    print("instance write", repr(s))
c.write = write2

l = [c, c2]

print(f(1, "evaluating argument"), file=f(l[0], "evaluating dest"))
print(f(2, "evaluating second argument"), file=l[0])
print(hasattr(c, "softspace"))
print(hasattr(c2, "softspace"))
print(f(3, 3), file=l[1])
print(hasattr(c2, "softspace"))
print(f(4, 4), file=l[0])
print(hasattr(c, "softspace"))

import sys
ss_1 = sys.stdout.softspace
print(1, end="")
ss_2 = sys.stdout.softspace
print()
ss_3 = sys.stdout.softspace
print(ss_1, ss_2, ss_3)

print(hasattr(c, "softspace"), file=c)
print()

class D(object):
    def __str__(self):
        return 1

try:
    print(D(), file=c)
    assert 0, "expected TypeError was not thrown"
except TypeError:
    pass

for i in xrange(10):
    print()
