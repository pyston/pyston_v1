import random

random.seed(12345)

def randchr():
    return chr(int(random.random() * 26) + ord('a'))
def randstr(n):
    return ''.join([randchr() for i in xrange(n)])

d = {}


def add():
    d[randstr(5)] = i

    print len(d)
    print d
    print d.items()

def pop():
    del d[d.keys()[0]]
    print len(d)
    print d
    print d.items()


for i in xrange(100):
    add()

print_final = d.values()

for i in xrange(100):
    pop()
    add()
    pop()

print print_final

print {'logoq': 349, 'kprzd': 301, 'qemgi': 342, 'xpuhv': 310, 'dpmbn': 250, 'trvxs': 264}
