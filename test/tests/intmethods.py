# can't try large numbers yet due to lack of long
for i in xrange(1, 100):
    for j in xrange(1, 100):
        print i.__divmod__(j)


for i in xrange(1, 12):
    for j in xrange(1, 12):
        print i | j
        print i & j
        print i ^ j

print 1 ** 0
print 0 ** 0
print -1 ** 0, (-1) ** 0, (-5) ** 0
print (11).__pow__(5, 50)
print (11).__pow__(32, 50)
print (11).__index__()

for i in (-10, 10, 0, -15):
    print i, i.__hex__(), i.__oct__()
    print i.bit_length()

# Testing int.__new__:
class C(int):
    def __init__(self, *args):
        print "C.__init__, %d args" % len(args)

class D(object):
    def __init__(self, n):
        self.n = n

    def __int__(self):
        return self.n

for a in (False, True, 2, 3.0, D(4), D(C(5)), D(False)):
    i = int.__new__(C, a)
    print type(i), i, type(int(a))

try:
    int.__new__(C, D(1.0))
except TypeError, e:
    print e

class I(int):
    pass

x = int(D(C()))
print type(x)

x = I(D(C()))
print type(x)

print type(int(C()))

print type(int(2**100))
print type(int(2L))
print type(int.__new__(int, 2**100))
print type(int.__new__(int, 2L))
try:
    print type(int.__new__(C, 2**100))
except OverflowError, e:
    print e
class L(object):
    def __int__(self):
        return 1L
print type(int(L()))

print int(u'123')
print int("9223372036854775808", 0)
print int("0b101", 2), int("0b101", 0)
print 1 << 63, 1 << 64, -1 << 63, -1 << 64, 2 << 63
print type(1 << 63), type(1 << 64), type(-1 << 63), type(-1 << 64), type(2 << 63)

for b in range(26):
    try:
        print int('123', b)
    except ValueError as e:
        print e
    try:
        print int(u'123', b)
    except ValueError as e:
        print e


class I(int):
    pass

for i1 in [1, I(2), 3, I(4)]:
    for i2 in [1, I(2), 3, I(4)]:
        print -i1, +i1, ~i1, i1 < i2, i1 <= i2, i1 == i2, i1 > i2, i1 >= i2, i1 != i2, i1 | i2, i1 ^ i2, i1 & i2, i1 * i2, i1 + i2, i1 / i2, i1 - i2, i1 ** i2, i1 // i2

print int("12345", base=16)

print type(2 ** 48)
