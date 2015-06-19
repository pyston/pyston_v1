def f(msg, rtn):
    print msg
    return rtn

n = 0
while n < 64:
    i = n & 3
    j = (n >> 2) & 3
    k = (n >> 4) & 3

    print f('a', i) and f('b', j)
    print f('a', i) or f('b', j)
    print f('a', i) and f('b', j) and f('c', k)
    print f('a', i) and f('b', j) or f('c', k)
    print f('a', i) or f('b', j) and f('c', k)
    print f('a', i) or f('b', j) or f('c', k)

    n = n + 1

class C(object):
    def __init__(self, x):
        self.x = x
    def __nonzero__(self):
        print "C.__nonzero__", self.x
        return self.x
    def __repr__(self):
        return "<C object>"

print hash(True) == hash(False)
print int(True), int(False)

c = C("hello") # This object has an invalid __nonzero__ return type
if 0:
    print bool(c) # this will fail
print 1 and c # Note: nonzero isn't called on the second argument!
print C(True) or 1 # prints the object repr, not the nonzero repr

print
# nonzero should fall back on __len__ if that exists but __nonzero__ doesn't:
class D(object):
    def __init__(self, n):
        self.n = n

    def __len__(self):
        print "__len__"
        return self.n

for i in xrange(0, 3):
    print i, bool(D(i))
