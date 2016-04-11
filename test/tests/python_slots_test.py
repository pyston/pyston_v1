print 'basic test'
class C(object):
    __slots__ = ['a', 'b', '__private_var']

c = C()
try:
    print c.a
except AttributeError as e:
    print e.message
c.a = 5
print c.a
print c.__slots__

c._C__private_var = 6
print c._C__private_var

try:
    c.x = 12
except AttributeError as e:
    print e.message

print 'testing __dict__'
class C(object):
    __slots__ = ['d', 'e', '__dict__']
c = C()
c.d = 5
print c.d
c.r = 6
print c.r
print c.__dict__.items() # dict should contain only r (not d)

print 'testing inheritance'
class C(object):
    __slots__ = ['a', 'b']
class D(object):
    __slots__ = ['c', 'd']
class E(object):
    pass

class G(C):
    __slots__ = ['k', 'l']
g = G()
g.a = 5
print g.a
g.k = 12
print g.k

class G(C, E):
    __slots__ = ['k', 'l']
g = G()
g.a = 5
print g.a
g.k = 12
print g.k

class G(E, C):
    __slots__ = ['k', 'l']
g = G()
g.a = 5
print g.a
g.k = 12
print g.k

try:
    class G(C, D):
        pass
except TypeError as e:
    print e.message

print 'testing a lot of slots'
class C(object):
    __slots__ = ['a' + str(i) for i in xrange(1000)]
c = C()
c.a0 = -8
print c.a0
for i in xrange(1000):
    setattr(c, 'a' + str(i), i)
for i in xrange(1000):
    print getattr(c, 'a' + str(i))

print 'slots on a subclass of str'
try:
    class C(str):
        __slots__ = ['a']
except TypeError as e:
    print e.message

print 'slots on a class with a metaclass'

class M(type):
    def __new__(*args):
        print "M.__new__", args[:3]
        return type.__new__(*args)

    def __call__(*args):
        print "M.__call__", args[:3]
        return type.__call__(*args)

class C(object):
    __metaclass__ = M
    __slots__ = ['a', 'b']
c = C()
c.a = 5
print c.a
c.b = 6
print c.b
