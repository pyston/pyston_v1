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
