# expected: fail
# - inheritance not implemented

class C(object):
    x = 1

class D(C):
    pass

d = D()
# When going from an instance, looking through the classes should look at base classes:
print d.x
# But also when doing instance-level lookups!
print D.x

print D.__dict__
