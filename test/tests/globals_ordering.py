# expected: fail
# - we don't order globals the same way as CPython

import random

random.seed(12345)

def randchr():
    return chr(int(random.random() * 26) + ord('a'))
def randstr(n):
    return ''.join([randchr() for i in xrange(n)])

d = {}


for i in xrange(20):
    globals()["attr_" + randstr(5)] = i
for k, v in globals().items():
    if not k.startswith("attr_"):
        continue
    print k, v


