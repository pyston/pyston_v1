class C(object):
    i = 5

    def __getattr__(self, attr):
        print '__getattr__ called on attr', attr
        return 6

c = C()
print c.i  # should print 5, not call __getattr__

class D(object):
    i = 5

class E(D):
    def __getattr__(self, attr):
        print '__getattr__ called on attr', attr
        return 6
    pass

e = E()
print e.i # should print 5, not call __getattr__
