l = range(5)
print l
print l * 5
l[0] = 1
print l
print l[2]
print l[2L]
l[:] = l

l = range(5)
while l:
    print l.pop()

l = range(7, -1, -2)
print sorted(l)
print l

l = range(5)
l[0] = l
print l

for i in xrange(-10, 10):
    l2 = range(5)
    l2.insert(i, 99)
    print i, l2

for i in xrange(-5, 4):
    l3 = range(5)
    print i, l3.pop(i), l3
print range(5).pop(2L)

for i in xrange(-5, 4):
    l3 = range(5)
    l3[:i] = [7, 8]
    print i, l3

print [1, 2, 3, 4, 5]

# test count method
l = [1, 2, 1, 2, 3]
print l.count(1)
print l.count(42)

print l.remove(1)
print l
try:
    l.remove(54)
    assert 0
except ValueError, e:
    print e
    print "ok"
print l

for i in xrange(5):
    l = range(i)
    l.reverse()
    print l

# list index
list_index = [1, 2, 3, 4, 5]
for i in xrange(1, 6):
    assert list_index.index(i) == i-1
    try:
        print list_index.index(i, 3, 4)
    except ValueError as e:
        print e
    try:
        print list_index.index(i, -1, -1)
    except ValueError as e:
        print e
        
assert list_index.index(3) == 2
assert [1, '2'].index('2') == 1

# growing and shrinking a list:
l = []
for i in xrange(100):
    l.append(i)
while l:
    del l[0]
    print l
for i in xrange(100):
    l.append(i)
while l:
    del l[0]

l = range(5)
l.extend(range(5))
print l

# Repeating a list
x = [0, 1, 2]
print 2 * x
print x * 2

print range(5) == range(5)
print range(5) == range(4)

class C(object):
    def __eq__(self, rhs):
        print "C.eq"
        return False

# Should not call C().__eq__
print [C()] == [1, 2]

l2 = l = range(5)
l3 = range(4)
l[:] = l3
print l, l2, l3
print l is l2
print l is l3

l = []
l[:] = range(5)
print l

for i in xrange(3):
    for j in xrange(5):
        l = range(i)
        l[j:] = ["added"]
        print i, j, l

        l = range(i)
        l[:j] = ["added"]
        print i, j, l

        for k in xrange(5):
            l = range(i)
            l[j:k] = ["added"]
            print i, j, k, l

def G():
    yield "a"
    yield "b"
    yield "c"
l = [0, 1, 2, 3, 4, 5]
l[1:] = G()
print l

l = [1, 3, 5, 7, 2, 4]
print l.sort(key=lambda x:x%3)
print l
print l.sort(reverse=True)
print l

# If the keyfunc throws an exception, we shouldn't see any modifications:
l = range(9)
try:
    print sorted(l, key=lambda i:1.0/(5-i))
except ZeroDivisionError:
    pass
print l

idxs = [-100, -50, -5, -1, 0, 1, 5, 50, 100]
for i1 in idxs:
    for i2 in idxs:
        l = range(10)
        del l[i1:i2]
        print i1, i2, l

l = []
del l[:]
print l

l = range(5)
l[2:4] = tuple(range(2))
print l


l = [None]*4
try:
    l[::-1] = range(5)
except ValueError as e:
    print e
l[::-1] = range(4)
print l
del l[::2]
print l

for i in xrange(3):
    for j in xrange(3):
        for k in xrange(3):
            l1 = [i]
            l2 = [j, k]
            print l1 < l2, l1 <= l2, l1 > l2, l1 >= l2


def mycmp(k1, k2):
    types_seen.add((type(k1), type(k2)))
    if k1 == k2:
        return 0
    if k1 < k2:
        return -1
    return 1

types_seen = set()
l = ["%d" for i in xrange(20)]
l.sort(cmp=mycmp)
print types_seen
print l

"""
types_seen = set()
l = range(20)
l.sort(cmp=mycmp, key=str)
print types_seen
print l
"""

print repr(list.__hash__)
