# TODO test all of this with getclsattr
# TODO should make an ics test

class C(object):
    def f():
        pass

def g():
    print 'running g'

print C.f == C.f
print C.f is C.f
print C().f == C().f
print C().f is C().f

#### Check the types of stuff

print type(C.f) # instancemethod
print type(C().f) # instancemethod
print type(g) # function

C.g = g
print type(C.g) # instancemethod
print type(C().g) # instancemethod

#### Assign a function to an instance

c = C()
c.g = g
print type(c.g) # function
c.g()

print c.g == c.g
print c.g is c.g

#### Assign a function to a class

def l(inst):
    print 'running l', inst.i
C.l = l
print type(C.l) #instancemethod
print type(C().l) #instancemethod
c1 = C()
c1.i = 1
C.l(c1)
c1.l()

print c1.l == c1.l
print c1.l is c1.l
print C.l == C.l
print C.l is C.l

#### Assign a bound instancemethod to a class

C.k = c1.l # set C.k to a bound instancemethod
C.k() # this should call l with with c1 as the arg
c2 = C()
c2.i = 2
c2.k() # this should just call l with c1 as the arg, not try to bind anything else
print type(C.k) # instancemethod
print type(c2.k) # instancemethod

print c2.k == c2.k
print c2.k is c2.k
print C.k == C.k
print C.k is C.k
print C.k == c2.k
print C.k is c2.k

#### Assign an unbound instancemethod to a class
#### Getting is will bind it like a normal function

# TODO implement instancemethod stuff so this case works
"""
C.m = C.l
print type(C.m) #instancemethod
print type(C().m) #instancemethod
c3 = C()
c3.i = 3
C.m(c3)
c3.m()

print c3.m == c3.m
print c3.m is c3.m
print C.m == C.m
print C.m is C.m
"""

### Assign a bound instancemethod to an instance

c4 = C()
c4.i = 4
c4.z = c1.l
print type(c4.z) # instancemethod
c4.z() # should call l(c1)

print c4.z == c4.z
print c4.z is c4.z

### Assign an unbound instancemethod to an instance

c4 = C()
c4.i = 4
c4.z = C.l
print type(c4.z) # instancemethod
c4.z(c1) # should call l(c1)

print c4.z == c4.z
print c4.z is c4.z

### Call a bound instancemethod on its own (not through the callattr path)
bound_instancemethod = c1.l
bound_instancemethod()
print type(bound_instancemethod)

### Call an unbound instancemethod on its own (not through the callattr path)
unbound_instancemethod = C.l
unbound_instancemethod(c2)
print type(unbound_instancemethod)

### Test instancemethod repr
print 'test instancemethod repr'
class C(object):
    def f(self):
        pass
    def __repr__(self):
        return '(alpacas are cool)'
print repr(C.f)
print repr(C().f)
