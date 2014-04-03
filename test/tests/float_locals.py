# Test having some float locals, which live in XMM registers, to exercise
# code paths that handle those.

class C(object):
    pass
C.x = 2.0
c = C()
c.x = 1.0

def getattr_test(c, n):
    t = 0.0
    for i in xrange(n):
        c.x
        t = t + 1.0
    return t

def setattr_test(c, n):
    t = 0.0
    for i in xrange(n):
        c.x = t
        t = t + 1.0
    return t

print getattr_test(c, 1000000)
print setattr_test(c, 1000000)

# Calling this with an empty C
# will cause the getattr to hit the class-level x attribute:
print getattr_test(C(), 1000000)

