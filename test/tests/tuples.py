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
