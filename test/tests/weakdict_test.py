from weakref import WeakKeyDictionary, WeakValueDictionary

class S(object):
    def __init__(self, n):
        self.n = n
    def __hash__(self):
        return hash(self.n)
    def __eq__(self, rhs):
        return self.n == rhs.n

def test(d):
    print "Testing on", d.__class__

    k, v = None, None
    for i in xrange(10):
        print i
        for j in xrange(100):
            k, v = S(0), S(0)
            d[k] = v
        import gc
        gc.collect()
        print len(d.keys()), k in d

test(WeakKeyDictionary())
test(WeakValueDictionary())
