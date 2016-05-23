# Some parsing tests:
print ((1, 2),)
print (1, 2, 3)
print (1,2,)
print (1,)

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

print () is (), () is tuple(), tuple() is tuple()

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

class MyTuple(tuple):
    pass
mt = MyTuple((1, 2))
print mt < (1, 2)
print (1, 2) < mt
print mt[1]
print mt + (1,)
print list(mt)
print len(mt)

# __add__
print () + ()
print (1, 2, 3) + ()
print () + (1, 2, 3)
print (1, 2) + (2, 3)

try:
    (1, 2) + "a"
except TypeError as e:
    print "adding failed"

## __new__
print tuple()
print tuple((1,3,7,42))
print tuple(['i', 42, 'j', 318])
print tuple('hello world')
print tuple({'a': 1})
print sorted(tuple({1,2,3,4}))

print tuple(sequence=(1,3,7,42))
print tuple(sequence=['i', 42, 'j', 318])
print tuple(sequence='hello world')
print tuple(sequence={'a': 1})
print sorted(tuple(sequence={1,2,3,4}))

print tuple((1,3,7,42)) == tuple(sequence=(1,3,7,42))
print tuple(['i', 42, 'j', 318]) == tuple(sequence=['i', 42, 'j', 318])
print tuple('hello world') == tuple(sequence='hello world')
print tuple({'a': 1}) == tuple(sequence={'a': 1})
print sorted(tuple({1,2,3,4})) == sorted(tuple(sequence={1,2,3,4}))

# too many arguments
try:
    tuple((1,2), (3,4))
except TypeError, e:
    print e

try:
    tuple((1,2), sequence=(3,4))
except TypeError, e:
    print e

try:
    tuple(sequence=(3,4), test='test', rest='rest')
except TypeError, e:
    print e

# invalid keyword argument for function
try:
    tuple(oops='invalid keyword')
except TypeError, e:
    print e

# __getitem__
t = (1, "2")
print t[0]
print t[1]
print t[1L]

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
print t[-1:-1:-5]
print t[-5:-4:-5]

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

print bool(())
print bool((1,))
print bool((0,))
print bool((0, 0))

print (65, (1, 2, 3), 65)

# Multiplying a tuple by an integer
x = (1, 2, 3)
print x * -1
print x * 0
print x * 1
print x * 5
print -1 * x
print 0 * x
print 1 * x
print 5 * x

x = ()
print x * -1
print x * 0
print x * 1
print x * 5
print -1 * x
print 0 * x
print 1 * x
print 5 * x

print (1, 3, 5, 3).index(3)
try:
    print (1, 3, 5, 3).index(2)
except ValueError as e:
    print e

n = float('nan')
print n in (n, n)

#recursive printing test
class C(object):
    def __init__(self):
        self.t = (self,)
    def __repr__(self):
        return repr(self.t)
print repr(C())

try:
    (1, 2) + "a"
except TypeError as e:
    print(type(e))

class D(object):
    def __rmul__(self, other):
        return other * 2

d = D()

try:
    print((1, 2) * 3.5)
except TypeError as e:
    print(type(e))

try:
    print((1, 2) * d)
except TypeError as e:
    print(e.message)

# this triggers a tuple resize because generators have a unknown len:
print len(tuple(v*10 for v in range(100)))
