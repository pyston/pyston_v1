print 123542598.12938712938192831293812983
f = 1.0
print f

print float(2)

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

try:
    f = float("hello world")
    print f
except ValueError as e:
    print e

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
