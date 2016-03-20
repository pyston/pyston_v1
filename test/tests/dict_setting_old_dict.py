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

c2 = C()
olddict = c2.__dict__
c2.__dict__ = { "should not be there" : 1 }
print olddict.has_key("should not be there")


