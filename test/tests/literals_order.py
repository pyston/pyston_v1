
class H(object):
    def __init__(self, n):
        self.n = n
        print "Creating", repr(n)

    def __hash__(self):
        print "hashing", repr(self.n)
        return self.n

    def __eq__(self, rhs):
        print "eq"
        return self is rhs

print "testing sets"
# Set literals evaluate all subexpressions, then do hash operatons:
def f():
    {H(1), H(2), H(1L)}
for i in xrange(100):
    f()


print "testing dicts"
# Dict literals evaluate subexpressions one by one, then do hash operations
def f2():
    {H(1): H(4), H(2): H(5), H(1L): H(6)}
for i in xrange(100):
    f2()
