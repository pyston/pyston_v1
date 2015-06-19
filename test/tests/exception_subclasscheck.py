# Exception-catching is supposed to go through __subclasscheck__

class MyException(Exception):
    pass

class M(type):
    def __instancecheck__(self, instance):
        print "instancecheck", instance
        return True

    def __subclasscheck__(self, sub):
        print "subclasscheck", sub

        if self.throw_on_subclasscheck:
            raise MyException()

        return True

class E(Exception):
    __metaclass__ = M
    throw_on_subclasscheck = False

class F(Exception):
    __metaclass__ = M
    throw_on_subclasscheck = True

print 1
print isinstance(E(), E) # does not print anything due to special-casing
print 2
print isinstance(1, E)
print 3

try:
    raise Exception()
except E:
    pass

print 4

# Exceptions in __subclasscheck__ should get ignored:
try:
    1/0
except F:
    print "shouldn't get here"
except ZeroDivisionError:
    print "ok"

