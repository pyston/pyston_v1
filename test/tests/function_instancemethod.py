# run_args: -n

class C(object):
    def f():
        pass

def g():
    print 'running g'

#### Check the types of stuff

print type(C.f) # instancemethod
print type(C().f) # instancemethod
print type(g) # function

C.g = g
print type(C.g) # instancemethod
print type(C().g) # instancemethod

#### Assign a function to an instance
#### It should stay a function

c = C()
c.g = g
print type(c.g) # function
c.g()

#### Assign a function to a class
#### It should become an unbound instancemethod

def l(inst):
    print 'running l', inst.i
C.l = l
print type(C.l) #instancemethod
print type(C().l) #instancemethod
c1 = C()
c1.i = 1
C.l(c1)
c1.l()

#### Assign a bound instancemethod to a class

C.k = c1.l # set C.k to a bound instancemethod
C.k() # this should call l with with c1 as the arg
c2 = C()
c2.i = 2
c2.k() # this should just call l with c1 as the arg, not try to bind anything else
print type(C.k) # instancemethod
print type(c2.k) # instancemethod

#### Assign an unbound instancemethod to a class
#### Behaves like assigning a function

C.m = C.l
print type(C.m) #instancemethod
print type(C().m) #instancemethod
c3 = C()
c3.i = 3
C.m(c3)
c3.m()

### Assign a bound instancemethod to an instance

c4 = C()
c4.i = 4
c4.z = c1.l
print type(c4.z) # instancemethod
c4.z() # should call l(c1)

### Assign an unbound instancemethod to an instance

c4 = C()
c4.i = 4
c4.z = C.l
print type(c4.z) # instancemethod
c4.z(c1) # should call l(c1)

### Call a bound instancemethod on its own (not through the callattr path)
bound_instancemethod = c1.l
bound_instancemethod()
print type(bound_instancemethod)

### Call an unbound instancemethod on its own (not through the callattr path)
unbound_instancemethod = C.l
unbound_instancemethod(c2)
print type(unbound_instancemethod)
