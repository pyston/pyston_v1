import _random
r = _random.Random(42)
print r.random()
s = r.getstate()
print r.getrandbits(100)
r.jumpahead(100)
print r.getrandbits(100)
r.setstate(s)
print r.getrandbits(100)

class Random(_random.Random):
    def __init__(self, a=None):
        super(Random, self).seed(a)

for i in xrange(100):
    r = Random(i)
    print r.getrandbits(100)
