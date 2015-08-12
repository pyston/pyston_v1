class C(object):
    def __init__(self, n):
        self.n = n

    def __hash__(self):
        print "__hash__"
        return self.n % 2

    def __eq__(self, rhs):
        print "__eq__", self.n + rhs.n
        return self.n % 2 == rhs.n % 2

    def __repr__(self):
        return "C(" + str(self.n) + ")"

d = {}
c1 = C(1)
c2 = C(2)
c3 = C(3)
d.__setitem__(c1, 1)
d.__setitem__(c2, 2)


# This will cause a dict colision.
# I'm not sure this is specified, but I happen to handle it the same way cPython does,
# which is to update the value but not the key, so the result should be that
# there is an entry from C(1) to 3
d.__setitem__(c3, 3)

print d


# dicts need to check identify and not just equality.
# This is important for sqlalchemy where equality constructs a sql equals clause and doesn't
# do comparison of the objects at hand.
d = {}
nan = float('nan')
d[nan] = "hello world"
print d[nan]



# Dicts should not check __eq__ for values that have different hash values,
# even if they internally cause a hash collision.
class C(int):
    def __eq__(self, rhs):
        print "eq", self, rhs
        raise Exception("Error, should not call __eq__!")

    def __hash__(self):
        print "hash", self
        return self

d = {}
for i in xrange(1000):
    d[C(i)] = i
print len(d)
