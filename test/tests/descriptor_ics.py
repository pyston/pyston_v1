# expected: fail
# - descriptors not implemented yet

# Make sure that we guard in a getattr IC to make sure that
# we don't subsequently get an object with a __get__ defined.

class D(object):
    pass

class C(object):
    d = D()

c = C()

def print_d(c):
    print c.d

print_d(c)
def get(self, obj, cls):
    print self, obj, cls
    return 1

D.__get__ = get
print_d(c)

# And also the other way around:
del D.__get__
print_d(c)

class E(object):
    def __get__(self, obj, cls):
        print "E.__get__"
        return 2
C.d = E()

# Or if we switch out the object entirely:
print_d(c)
