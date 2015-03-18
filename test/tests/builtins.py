__builtins__.aoeu = 1
print aoeu

__builtins__.True = 2
print True
print bool(1)
print bool(1) is True

__builtins__.__builtins__ = 1
print __builtins__

__builtins__ = 2
print __builtins__

print all([]), all([True]), all([False]), all([None]), all([True, False, None])
print any([]), any([True]), any([False]), any([None]), any([True, False, None])

print sum(range(5))
print sum(range(5), 5)

class C(object):
    def __init__(self, n):
        self.n = n
    def __add__(self, rhs):
        self.n = (self.n, rhs.n)
        return self

print sum([C(1), C(2), C(3)], C(4)).n

print zip([1, 2, 3, 0], ["one", "two", "three"])

print filter(lambda x: x % 2, xrange(20))
print type(enumerate([]))
print list(enumerate(xrange(5, 10)))

# If the first argument is None, filter calls checks for truthiness (ie is equivalent to passing 'bool')
print filter(None, xrange(-5, 5))

print isinstance(1, int)
print isinstance(1, (float, int))
print isinstance(1, (float, (), (int, 3), 4))

print divmod(5, 2)
print divmod(5L, -2)
try:
    divmod(1, "")
except TypeError, e:
    print e

def G():
    yield "A"; yield "B"; yield "C"
print list(enumerate(G()))

print next(iter([]), "default")
print next(iter([]), None)
print next(iter([1]), "default")

class C(object):
    def __init__(self):
        self.a = 1
print vars(C()).items()

print globals().get("not a real variable")
print globals().get("not a real variable", 1)

print hex(12345)
print oct(234)
print hex(0)
print oct(0) # This should not add an additional leading 0, ie should return "0" not "00"

try:
    print hex([])
except TypeError, e:
    print e

class Iterable(object):
    def __iter__(self):
        return self
    def next(self):
        return 1

i = Iterable()
it = iter(i)
print it is i

# check that builtins don't bind
class C(object):
    s = sorted
c = C()
print c.s([3,2,1])

l = range(5)
print sorted(l, key=lambda x:-x)
print l

print bytes
print bytes is str
print repr(b'1234')

print callable(1)
print callable(int)
print callable(lambda: 1)

print range(5L, 7L)

print round(-1.1), round(-1.9)
print round(0.5), round(-0.5)

print list(iter(xrange(100).__iter__().next, 20))

print bytearray(xrange(256))
