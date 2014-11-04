# expected: fail
# - don't support this stuff yet

# Exception-catching is supposed to go through __subclasscheck__

class M(type):
    def __instancecheck__(self, instance):
        print "instancecheck", instance
        return True

    def __subclasscheck__(self, sub):
        print "subclasscheck", sub
        return True

class E(Exception):
    __metaclass__ = M

print 1
print isinstance(E(), E) # does not print anything due to special-casing
print 2
print isinstance(1, E)
print 3

# This calls __subclasscheck__ twice...?
try:
    raise E()
except E:
    pass

print 4

try:
    raise Exception()
except E:
    pass

print 5
