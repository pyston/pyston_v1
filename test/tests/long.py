l = 2L
print l
print type(l)

t = 1L
for i in xrange(150):
    t *= l
    print t, repr(t)

def test(a, b):
    print repr(a), repr(b)
    print a + b, b + a, a.__add__(b), b.__add__(a)
    print a - b, b - a, a.__sub__(b), b.__sub__(a)
    print a * b, b * a, a.__mul__(b), b.__mul__(a)
    print a / b, b / a, a.__div__(b), b.__div__(a)
    print a % b, b % a, a.__mod__(b), b.__mod__(a)
    print repr(a), repr(b), a < b, a > b, a <= b, a >= b, a == b, a != b
    if not isinstance(a, float) and not isinstance(b, float):
        print a ^ b, a | b, a & b
        print a.__hex__(), b.__hex__(), a.__oct__(), b.__oct__()
        print a // b, b // a, a.__floordiv__(b), b.__floordiv__(a)


print 1L / 5L
print -1L / 5L
print 1L / -5L
print -1L / -5L

for a in [-5, -1, 1, 5, -2L, -1L, 1L, 2L, 15L]:
    for b in [-5, -1, 1, 5, -2L, -1L, 1L, 2L]:
        test(a, b)

test(1L, 2.0)
test(3.0, 2L)

for lhs in [2L, -2L]:
    for rhs in [-1, -1L, 1, 2L]:
        print lhs.__rdiv__(rhs), lhs.__truediv__(rhs), lhs.__rtruediv__(rhs)

print (1L) << (2L)
print (1L) << (2)
print (1) << (1L)
print (1) << (128L)
try:
    print (1L) << (-1L)
except ValueError, e:
    print e
try:
    print (1L) << (-1)
except ValueError, e:
    print e

print ~(1L)
print ~(10L)
print ~(-10L)

print -(1L)
print 1L**2L
print 1L**2
print 0 ** (1 << 100)
print pow(1 << 30, 1 << 30, 127)
print pow(1L << 30, 1L << 30, 127)
print pow(1 << 100, 1 << 100, 1000)
print pow(-1, (1<<100))
print pow(-1, (1<<100) + 1)
print pow(0, (1<<100))
print pow(1, (1<<100))
print pow(5, 3, -7L)
print pow(-5,  3, 7L)
print pow(-5,  3, -7L)
print (11L).__pow__(32, 50L)
print (11L).__index__()

print long("100", 16)
print long("100", 10)
print long("100", 26)
print long("0x100", 16), long("0100", 8), long("0b100", 2)
print long("0x100", 0), long("0100", 0), long("0b100", 0)
print long("0b100", 16), long("0b100L", 0), long("-0b100L", 0)
print long(-1.1)
print long(1.9)
print long(-1.9)

print type(hash(1L))
print hash(1L) == hash(2L)

# Testing long.__new__:
class C(long):
    def __init__(self, *args):
        print "C.__init__, %d args" % len(args)

class D(object):
    def __init__(self, n):
        self.n = n

    def __long__(self):
        return self.n

for a in (False, True, 2, 3L, D(4), D(C(5)), D(False)):
    i = long.__new__(C, a)
    print type(i), i, type(long(a))

try:
    long.__new__(C, D(1.0))
except TypeError, e:
    print e

class I(long):
    pass

x = long(D(C()))
print type(x)

x = I(D(C()))
print type(x)

print type(long(C()))

print repr(int("123456789123456789123456789", 16))

a = 2389134823414823408429384238403228392384028439480234823
print +a
print +long.__new__(C, 5L)
print type(+long.__new__(C, 5L))
