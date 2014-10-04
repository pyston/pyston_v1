# Regression test:
# If the init function doesn't exist, shouldn't just silently ignore any args
# that got passed

class C(object):
    def __new__(cls, args):
        return object.__new__(cls)

c = C(1)
print "This should have worked"

class D(object):
    pass

d = D(1)
print "This should have failed"
