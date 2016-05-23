print 123542598.12938712938192831293812983
f = 1.0
print f

print float(2)
print float(x=2)

# print f - 2

class F(float):
    def __init__(self, *args):
        print "F.__init__, %d args" % len(args)

f = F(1)
print f is float(f)
print type(float(f))
print float(f)

class D(object):
    def __init__(self, d):
        self.d = d

    def __float__(self):
        return self.d

for a in (1.0, D(2.0), F(), D(F())):
    f = float(a)
    print f, type(f)
    f = float.__new__(F, a)
    print f, type(f)

print type(float(D(F())))

class F2(float):
    pass

print type(F2(D(F())))

print type(float(F()))

for a in ["hello world", None]:
    try:
        f = float(a)
        print f
    except ValueError as e:
        print "ValueError", e
    except TypeError as e:
        print "TypeError", e

try:
    f = float("5 hello world")
    print f
except ValueError as e:
    pass
    # We don't print the right thing yet:
    # print e

print '__getformat__ test'
print float.__getformat__('double')
print float.__getformat__('float')
try:
    float.__getformat__('oooga booga boooga')
except Exception as e:
    print e.message

print float.fromhex("f0.04a")
print (5.0).hex()
print (0.5).as_integer_ratio()
print (0.5).is_integer()
print (1.0).is_integer()
print 1.0.__hash__(), 1.1.__hash__(), -1.1.__hash__()
print (0.5).conjugate(), (0.6).imag

print 1.0 ** (10 ** 100)
print (-1.0) ** (10 ** 100)
print (-1.0) ** (10 ** 100 + 1)
print 0.0 ** 0.0
try:
    0.0 ** (-1.0)
except ZeroDivisionError as e:
    print e.message

print float(1l)
print float(0l)
print float(-1l)
print (1l).__float__()
l = 1024 << 1024
try:
    float(l)
except OverflowError as e:
    print e.message
try:
    float(-l)
except OverflowError as e:
    print e.message

print 0.0
print -0.0
print -(0.0)
print -(-0.0)

print repr((1e100).__trunc__())

all_args = [None, 1, 1L, -1, -1L, 2.0, 0.5, 0, "",
            0.0, -0.0, -0.5, float('nan'), float('inf')]
for lhs in all_args:
    for rhs in all_args:
        for mod in all_args:
            try:
                print pow(lhs, rhs, mod)
            except Exception as e:
                print type(e), e

import sys
print sys.float_info

if 1:
    x = -2.0

print(float.__long__(sys.float_info.max))
print(float.__int__(sys.float_info.max))

data = ["-1.0", "0.0", "1.0",
        "5.0", "-5.0",
        "5", "5L", "0L", "5+5j",
        "\"5\"", "None",
        ]

operations = ["__rpow__",
              "__ridv__",
              "__divmod__", "__rdivmod__",
              "__rtruediv__",
              "__coerce__"
              ]

for x in data:
    for y in data:
        for operation in operations:
            try:
                print(eval("float.{op}({arg1}, {arg2})".format(op=operation,
                                                               arg1=x,
                                                               arg2=y)))
            except Exception as e:
                print(e.message)


class Foo1(float):
    def __rdiv__(self, other):
        print("float custom operation called")
        return self / other


class Foo2(long):
    def __rdiv__(self, other):
        print("long custom operation called")
        return self / other


class Foo3(int):
    def __rdiv__(self, other):
        print("int custom operation called")
        return self / other

a = Foo1(1.5)
b = Foo2(1L)
c = Foo3(1)

print(1.5 / a)
print(1.5 / b)
print(1.5 / c)
print(1 / a)
print(1 / b)
print(1 / c)
print(1L / a)
print(1L / b)
print(1L / c)
