# Setting __getattribute__ on a base class should invalidate ICs for subclasses

class B(object):
    pass

class C(B):
    pass

def getattribute(self, attr):
    return 1

c = C()
b = B()
c.a = 0
b.a = 0
for i in xrange(150):
    print i, b.a, c.a

    if i == 120:
        B.__getattribute__ = getattribute
