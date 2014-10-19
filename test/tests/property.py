class C(object):
    def fget(self):
        return 5

    def fset(self, val):
        print 'in fset, val = ', val

    x = property(fget, fset)

c = C()
print c.x
c.x = 7
print c.x
