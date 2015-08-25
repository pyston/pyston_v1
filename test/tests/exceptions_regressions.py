class C(object):
    def f(self, n):
        if n == 5000:
            return
        else:
            raise Exception()

def f(c, i):
    try:
        c.f(i)
    except Exception:
        pass

c = C()
for i in xrange(10000):
    f(c, i)
