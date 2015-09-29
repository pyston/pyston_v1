# doing object.__getattribute__(cls, "foo") should *not* do the normal
# special rules for looking things up on types: it should not check base
# classes, and it should not run descriptors.

class Descr(object):
    def __get__(*args):
        print "Descr.__get__"
        return 1

class C(object):
    d = Descr()

class E(C):
    pass

# Test that it doesn't execute descriptors:
print type(object.__getattribute__(C, 'd')) # Descr
print type(type.__getattribute__(C, 'd')) # int

# Test that it doesn't look at base classes:
print type(E.d) # Descr
try:
    print type(object.__getattribute__(E, 'd'))
    assert 0
except AttributeError as e:
    print e


try:
    print type.__getattribute__(1, 'd')
    assert 0
except TypeError as e:
    print e


# The exception messages are slightly different:
try:
    type.__getattribute__(C, 'x')
    assert 0
except AttributeError as e:
    print e
try:
    object.__getattribute__(C, 'x')
    assert 0
except AttributeError as e:
    print e
