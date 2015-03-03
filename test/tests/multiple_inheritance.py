# Testing the basic multiple-inheritance rules and functionality:

class C(object):
    n = 1
    def c(self):
        print "C.c()"

    def f(self):
        print "C.f()", self.n

class D(object):
    n = 2
    def d(self):
        print "D.d()"

    def f(self):
        print "D.f()", self.n

class E(C, D):
    pass
class F(D, C):
    pass
class G(E, D):
    pass

for o in [C(), D(), E(), F()]:
    print type(o), type(o).mro(), type(o).__mro__
    o.f()
    try:
        o.c()
    except Exception, e:
        print e
    try:
        o.d()
    except Exception, e:
        print e


# Testing invalid multiple inheritance hierarchies:

# Conflicting MRO:
try:
    class G(E, F):
        pass
except TypeError, e:
    # This is silly but I think actually reasonable: the error message for this case is implementation-specific,
    # since it depends on dict ordering rules (says either "for bases E, F" or "for bases F, E", depending on ordering).
    # Canonicalize the error message by sorting the characters in it:
    print ''.join(sorted(e.message)).strip()

# Conflicting MRO:
try:
    class H(C, F):
        pass
except TypeError, e:
    print ''.join(sorted(e.message)).strip()

# "instance lay-out conflict"
try:
    class I(str, int):
        pass
except TypeError, e:
    print e

# Testing the way super() behaves with multiple inheritance:

class S(C, D):
    n = 3
    def c(self):
        print "S.c()"
        super(S, self).c()

    def d(self):
        print "S.d()"
        super(S, self).d()

    def f(self):
        print "S.f()"
        print self.n, super(S, self).n, super(C, self).n
        # TODO: Pyston doesn't support the full super.__getattribute__, so this will abort
        # rather than throw an exception:
        # try:
            # print super(D, self).n
        # except Exception as e:
            # print e
        super(S, self).f()
        super(C, self).f()
s = S()
s.c()
s.d()
s.f()

for cls in [object, tuple, list, type, int, bool]:
    print cls.__mro__
