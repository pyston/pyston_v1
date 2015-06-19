class C(object):
    def __setitem__(self, k, v):
        print k, v
        self.k = k
        self.v = v
        return "hello"

c = C()
c[1] = 2
print c.k
print c.v

def si(k, v):
    print "bad si!"
    o

c.__setitem__ = si
c[3] = 4

try:
    1[2] = 3
except TypeError, e:
    print e
