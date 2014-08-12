t = (1, "h")
print t, str(t), repr(t)
if 1:
    t = (3,)
print t

def f():
    t = (1, 3)
    print t
f()

print ()
print (1,)
print (1, 2)
print (1, 2, 3)

t = 1, 3
print t

print (2,) < (2,)
print (2,) < (2, 3)
print (3,) < (2, 3)

print

class T(object):
    def __init__(self, n):
        self.n = n

    def __lt__(self, rhs):
        print "lt", self.n, rhs.n
        return self.n < rhs.n

    def __le__(self, rhs):
        print "le", self.n, rhs.n
        return self

    def __gt__(self, rhs):
        print "gt", self.n, rhs.n
        return False

    def __ge__(self, rhs):
        print "ge", self.n, rhs.n
        return False

    def __eq__(self, rhs):
        print "eq", self.n, rhs.n
        return self.n == rhs.n

    def __hash__(self):
        return hash(self.n)

    def __repr__(self):
        return "<T>"

def t(l, r):
    print l < r
    print l <= r
    print l > r
    print l >= r
    print l == r
    print l != r
    print "same hash: " , hash(l) == hash(r)

t(T(1), T(2))
t(T(1), T(1))
t((T(1),), (T(1),))
t((T(1),), (T(2),))
t((T(1),1), (T(2),))
t((T(1),), (T(2),1))

print ("hello", "world", ["test"])

# __add__
print () + ()
print (1, 2, 3) + ()
print () + (1, 2, 3)
print (1, 2) + (2, 3)

# __getitem__
t = (1, "2")
print t[0]
print t[1]

t = (1, 2, 'a', 'b', 'c')
print t[::-1]
print t[:-1]
print t[0:2]
print t[-5:]
print t[-5:3]
print t[-5:10]
print t[:-5]
print t[:3]
print t[:10]
print t[1:3:-1]
print t[3:1:-1]
print t[1:3:1]
print t[1:3:2]
print t[1:5:3]
print t[5:1:-1]
print t[5:1:-2]
print t[5:1:-5]
print t[5:1]

try:
    t[None]
except TypeError as e:
    print e

try:
    t[(1, 2)]
except TypeError as e:
    print e

# Single element indexing.
t = (1, None, "abc")
for n in range(-3, 3):
    print t[n]

try:
    t[-4]
except IndexError as e:
    print e

try:
    t[3]
except IndexError as e:
    print e
