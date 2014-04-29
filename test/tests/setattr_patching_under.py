# expected: fail
# - finalization not implemented yet
# This test might also be broken in the presence of GC

class C(object):
    pass

def set(o, a):
    o.x = a

class D(object):
    def __del__(self):
        print "in __del__"
        c = C()
        c.a = 1
        c.b = 2
        c.c = 3
        c.d = 4
        c.e = 5
        c.f = 6
        c.g = 7
        c.h = 8
        set(c, 1)
        print "done with __del__"

c = C()

# The first set() just adds the attribute
print 1
set(c, 1)

# The second set() rewrites the setattr to be a in-place set, and also adds the D object
print 2
set(c, D())

# This third set() will remove the D() object, so by the time the set() finishes,
# the patchpoint could be rewritten due to the set() in the D.__del__ destructor
print 3
set(c, 1)

print 4
