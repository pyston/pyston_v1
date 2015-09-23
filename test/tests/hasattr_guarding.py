# Test some weird hasattr guarding scenarios
# I think this applies equally well to getattr

def f(o):
    print hasattr(o, "a")
    print getattr(o, "a", None)

class C(object):
    def __getattr__(self, key):
        print "getattr", key
        raise AttributeError(key)

for i in xrange(300):
    print i
    c = C()
    try:
        f(c)
    except AttributeError as e:
        print e
    c.a = 1
    setattr(c, str(i), i)
    f(c)

