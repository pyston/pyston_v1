# Test a relatively-obscure corner case of multiple inheritance:
# the metaclass is normally the first base's metaclass, but type_new
# will specifically delegate to a later base's metaclass if it is more
# derived.

# First, test what I'm guessing is the common case, where a later
# class is the only one with a non-default metaclass:

class C(object):
    pass

class M(type):
    def __new__(cls, *args):
        print "M.__new__", cls
        return type.__new__(cls, *args)

    def __init__(self, *args):
        print "M.__init__"
        return type.__init__(self, *args)

class D(object):
    __metaclass__ = M
print type(D)

class E(C, D):
    pass
print type(E)


# Then, test to make sure that it's actually type_new that's doing this, and not
# the class creation machinery.  We can check this by using an initial metatype that
# doesn't defer to type_new

class GreedyMeta(type):
    def __new__(cls, name, bases, attrs):
        print "GreedyMeta.__new__", cls
        if 'make_for_real' in attrs:
            return type.__new__(cls, name, bases, attrs)
        return 12345

class F(object):
    __metaclass__ = GreedyMeta
    make_for_real = True

print F, type(F)

class G(F, D):
    pass
print G

# Constructing the class with the bases in the opposite order will fail,
# since this will end up calling M.__new__ -> type.__new__, and type_new
# does some extra checks, that we skipped with GreedyMeta.
try:
    class H(D, F):
        pass
except TypeError as e:
    print e

