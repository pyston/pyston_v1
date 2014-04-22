# This microbenchmark is inspired by the ICBD type checker, which has a "scoring" phase at
# the end of the analysis.
# I'm not sure how representative this file is of the type checker, or even if
# this is really a polymorphism test or maybe just a tree test, or some cross of the two.

class Union(object):
    def __init__(self, subs):
        self.subs = subs

    def score(self):
        t = 0
        for s in self.subs:
            t += s.score()
        t /= len(self.subs) ** 2.0
        return t

class Simple(object):
    def score(self):
        return 1.0

class Poly1(object):
    def __init__(self, sub):
        self.sub = sub

    def score(self):
        return self.sub.score()

d = 0.0
def rand():
    # Almost cryptographically secure?
    global d
    d = (d * 1.24591 + .195) % 1
    return d

def make_random(x):
    if rand() > x:
        return Simple()

    if rand() < 0.3:
        return Union([make_random(0.5 * (x - 1)), make_random(0.5 * (x - 1))])
    return Poly1(make_random(x - 1))

# Create a 10k-ary tree, and score it 1k times
r = make_random(10000)
for i in xrange(1000):
    r.score()

# Other test configurations that can be run:

# Test 1: one 100k-ary tree:
# make_random(100000).score()

# Test 2: 1k 1k-ary trees:
# for i in xrange(1000):
    # make_random(1000).score()

# Test 3: one 1k-ary tree, 1k times:
# r = make_random(1000)
# for i in xrange(1000):
    # r.score()

# Test 4: 10k 100-ary trees:
# for i in xrange(10000):
    # make_random(100).score()

# Test 5: one 100-ary tree, 10k times::
# r = make_random(100)
# for i in xrange(10000):
    # r.score()

# Test 6: 100k 10-ary trees:
# for i in xrange(100000):
    # make_random(10).score()

