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
