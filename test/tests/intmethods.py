# can't try large numbers yet due to lack of long
for i in xrange(1, 100):
    for j in xrange(1, 100):
        print i.__divmod__(j)


for i in xrange(1, 12):
    for j in xrange(1, 12):
        print i | j
        print i & j
        print i ^ j
        print 1 | 2, 1 & 2, 1 & 2

print (2).__int__()
print (True).__int__()
print (2).__trunc__()
print (True).__trunc__()

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
    print int(None)
except TypeError, e:
    print e
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

class I(int):
    def __init__(self, n):
        print "I.__init__(%r)" % n
        self.n = n

    def __int__(self):
        return self

    def __repr__(self):
        return "<I(%r)>" % self.n

def call_int(i):
    print "calling int(%r)" % i
    i2 = int(i)
    print "return type:", type(i2)

print
call_int(1)

print
i = I(1)
call_int(i)
print "i.n is a", type(i.n) # should be 'I' now!

print
del I.__int__
i = I(1)
call_int(i)

print

# These return longs:
print int("12938719238719827398172938712983791827938712987312")
print int(u"12938719238719827398172938712983791827938712987312")
print int("12938719238719827398172938712983791827938712987312", 16)
print int(u"12938719238719827398172938712983791827938712987312", 16)
print int(1e100)
print int(*[1e100])
print int(x=1e100)

import sys
min_int = -1 - sys.maxint
max_int = sys.maxint

print(min_int / 2)
print(min_int / -2)
print(min_int / 3)
print(min_int / -3)
print(min_int / 5)
print(min_int / -5)
print(min_int / 7)
print(min_int / -7)

print(max_int / 2)
print(max_int / -2)
print(-max_int / 2)
print(max_int / 3)
print(max_int / -3)
print(-max_int / 3)
print(max_int / 5)
print(max_int / -5)
print(-max_int / 5)
print(max_int / 7)
print(-max_int / 7)
print(max_int / -7)

try:
    int(x=10, base=16)
except TypeError as e:
    print(e.message)

if sys.version_info >= (2, 7, 6):
    try:
        int(base=16)
    except TypeError as e:
        print(e.message)
else:
    print("int() missing string argument")

pow_test_data = [42, 3, 3L, 4.5, "x", 0, -42, None]

for rhs in pow_test_data:
    for lhs in pow_test_data:
        for mod in pow_test_data:
            try:
                print(int.__rpow__(rhs, lhs, mod))
            except Exception as e:
                print(e.message)

unary_test_data = [-42, -0, 0, 42, max_int, min_int]

for i in unary_test_data:
    print(int.__abs__(i))
    print(int.__long__(i))
    print(int.__float__(i))

data = ["-1", "0", "1",
        "5", "-5",
        "5.0", "5L", "0L", "5+5j", "0.0",
        "\"5\"", "None",
        ]

operations = ["__radd__",
              "__rand__",
              "__ror__",
              "__rxor__",
              "__rsub__",
              "__rmul__",
              "__rdiv__",
              "__rfloordiv__",
              "__rpow__",
              "__rmod__",
              "__rdivmod__",
              "__rtruediv__",
              "__rrshift__",
              "__rlshift__",
              "__coerce__",
              ]

for x in data:
    for y in data:
        for operation in operations:
            try:
                print(eval("int.{op}({arg1}, {arg2})".format(op=operation,
                                                               arg1=x,
                                                               arg2=y)))
            except Exception as e:
                print(e.message)
