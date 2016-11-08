# expected: fail
# - we don't order attributes the same way as CPython

import random

random.seed(12345)

def randchr():
    return chr(int(random.random() * 26) + ord('a'))
def randstr(n):
    return ''.join([randchr() for i in xrange(n)])

d = {}


class C(object):
    pass

for i in xrange(20):
    setattr(C, "attr_" + randstr(5), i)
for k, v in C.__dict__.items():
    if not k.startswith("attr_"):
        continue
    print k, v


