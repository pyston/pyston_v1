g = 0
class C(object):
    @property
    def f(self):
        print "f"
        if g:
            raise AttributeError()

c = C()

for i in xrange(100):
    print i, getattr(c, 'f', 0)

    if i == 50:
        g = 1

