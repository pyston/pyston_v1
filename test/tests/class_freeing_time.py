# expected: fail
# - __del__ not supported
# probably need to have some gc collections going on as well

# Classes should be freed right away

class DeallocShower(object):
    def __init__(self, n):
        self.n = n
    def __del__(self):
        print "del", self.n

class C(object):
    pass
c = C()
c.d = DeallocShower(1)
C.d2 = DeallocShower(2)
print 1
C = None
print 2
c = None
# C can be freed here I believe
print 3
# But cPython doesn't dealloc it until program termination
