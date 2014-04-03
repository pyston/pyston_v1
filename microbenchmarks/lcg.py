class Random(object):
    def __init__(self, seed):
        self.cur = seed

    def next(self):
        self.cur = (self.cur * 1103515245 + 12345) % (1 << 31)
        return self.cur

def f():
    r = Random(0)
    t = 0
    for i in xrange(10000000):
        t = t + r.next()
    print t
f()
