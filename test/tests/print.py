# This is the same as print_function.py, but using print as a statement

def f(o, msg):
    print msg
    return o

class C(object):
    def write(self, s):
        print "class write", repr(s)

c = C()
c2 = C()

def write2(s):
    print "instance write", repr(s)
c.write = write2

l = [c, c2]

print >>f(l[0], "evaluating dest"), f(1, "evaluating argument"),
print >>l[0], f(2, "evaluating second argument"),
print "softspace:", c.softspace
print hasattr(c2, "softspace")
print >>l[1], f(3, 3),
print hasattr(c2, "softspace")
print >>l[0], f(4, 4)
print "softspace:", c.softspace

import sys
ss_1 = sys.stdout.softspace
print 1,
ss_2 = sys.stdout.softspace
print
ss_3 = sys.stdout.softspace
print ss_1, ss_2, ss_3

print >>c, c.softspace, c.softspace
print

def clear_softspace():
    print c.softspace
    print >>c
    print c.softspace
    return 2
# there is no softspace before the print value of clear_softspace(), even though it looks like there should be
print >>c, c.softspace, clear_softspace(), c.softspace

class D(object):
    def __str__(self):
        return 1

try:
    print >>c, D()
    assert 0, "expected TypeError was not thrown"
except TypeError:
    pass

print >>None, "this should still print"
try:
    print >>1, "this should error"
except AttributeError as e:
    print e
