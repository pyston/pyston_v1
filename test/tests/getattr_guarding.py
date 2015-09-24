# Test some weird getattr/hasattr guarding scenarios


# Make sure that we guard correctly when a __getattr__ is involved:
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


# Make sure that we guard correctly when we are megamorphic
class C(object):
    pass
l = []
for i in xrange(200):
    c = C()
    setattr(c, "a%d" % i, i)

    # We should do the right guarding so that when i==150 comes around, this will return True/None:
    print i, hasattr(c, "a150"), getattr(c, "a150", None)
