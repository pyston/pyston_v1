import _random
r = _random.Random(42)
print r.random()
s = r.getstate()
print r.getrandbits(100)
r.jumpahead(100)
print r.getrandbits(100)
r.setstate(s)
print r.getrandbits(100)
