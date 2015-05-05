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
