# fail-if: '-x' in EXTRA_JIT_ARGS
# I think pypa has an issue parsing decorator expressions if they aren't simple names
# https://github.com/vinzenz/libpypa/issues/15

class C(object):
    def fget(self):
        return 5

    def fset(self, val):
        print 'in fset, val =', val

    x = property(fget, fset)

c = C()
print c.x
print C.x.__get__(c, C)
print type(C.x.__get__(None, C))
c.x = 7
print c.x

class C2(object):
    @property
    def x(self):
        print "x1"
        return 2

    x1 = x

    @x.setter
    def x(self, value):
        print "x2"
        return 3

    x2 = x

    @x.deleter
    def x(self):
        print "x3"

c = C2()

print "These should all succeed:"
print c.x1
print c.x2
print c.x

try:
    # This will fail since x1 is a copy that didn't have the setter set:
    c.x1 = 1
except AttributeError, e:
    print e
c.x2 = 1
c.x = 1


try:
    # This will fail since x1 is a copy that didn't have the deleter set:
    del c.x1
except AttributeError, e:
    print e
try:
    # This will fail since x1 is a copy that didn't have the deleter set:
    del c.x2
except AttributeError, e:
    print e
c.x = 1
