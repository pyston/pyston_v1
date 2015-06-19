# expected: fail
# - haven't bothered to implement this yet

# Funny case: if we get the attrwrapper of an object, then change it to be dict-backed,
# the original attrwrapper should remain valid but no longer connected to that object.

class C(object):
    pass

c1 = C()
aw = c1.__dict__
c1.a = 1
print aw.items()
c1.__dict__ = d = {}
print aw.items()
