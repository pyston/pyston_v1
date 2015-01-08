# can't try large numbers yet due to lack of long
for i in xrange(1, 100):
    for j in xrange(1, 100):
        print i.__divmod__(j)


for i in xrange(1, 12):
    for j in xrange(1, 12):
        print i | j
        print i & j
        print i ^ j

print 1 ** 0
print 0 ** 0
print -1 ** 0



# Testing int.__new__:
class C(int):
    pass

class D(object):
    def __init__(self, n):
        self.n = n

    def __int__(self):
        return self.n

for a in (False, True, 2, 3.0, D(4), D(C(5)), D(False)):
    i = int.__new__(C, a)
    print type(i), i, type(int(a))

try:
    int.__new__(C, D(1.0))
except TypeError, e:
    print e
