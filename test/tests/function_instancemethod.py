class C(object):
    def f():
        pass

def g(): pass

print type(C.f) # instancemethod
print type(C().f) # instancemethod
print type(g) # function

C.g = g
print type(C.g) # instancemethod
print type(C().g) # instancemethod

c = C()
c.g = g
print type(c.g) # function
