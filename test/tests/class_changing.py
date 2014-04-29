# expected: fail
# - class changing not supported yet

# Tests to make sure that setting __class__ changes the class, and that it's ok to disallow
# having anything other than a type as the class
class C(object):
    def foo(self):
        print "C.foo()"

class D(object):
    def foo(self):
        print "D.foo()"

c = C()
c.foo()

c.__class__ = D
c.foo()

# This should err:
c.__class__ = 1
