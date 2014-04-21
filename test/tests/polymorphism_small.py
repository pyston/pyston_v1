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
        return Union([make_random(0.5 * x - 1), make_random(0.5 * x - 1)])
    return Poly1(make_random(x - 1))

print make_random(1000).score()
