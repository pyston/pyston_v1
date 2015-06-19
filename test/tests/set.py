s1 = {1}

def sorted(s):
    l = list(s)
    l.sort()
    return repr(l)

s1 = set() | set(range(3))
print sorted(s1)
s2 = set(range(1, 5))
print sorted(s2)

print repr(sorted(s1)), str(sorted(s1))

print sorted(s1 - s2)
print sorted(s2 - s1)
print sorted(s1 ^ s2)
print sorted(s1 & s2)
print sorted(s1 | s2)

print len(set(range(5)))

s = set(range(5))
print sorted(s)
s.add(3)
print sorted(s)
s.add("")
print len(s)
s.add(None)
print len(s)

print set([1])

for i in set([1]):
    print i




s = frozenset(range(5))
print len(s)

print sorted(s)
print frozenset()

print hasattr(s, "remove")
print hasattr(s, "add")

print frozenset() | frozenset()
print set() | frozenset()
print frozenset() | set()
print set() | set()

for i in xrange(8):
    print i, i in set(range(2, 5))
    print i, i in frozenset(range(2, 5))

s = set(range(5))
print len(s)
s.clear()
print s

s.update((10, 15))
print sorted(s)
s.update((10, 15), range(8))
print sorted(s)
s.remove(6)
print sorted(s)
try:
    s.remove(6)
except KeyError, e:
    print e

def f2():
    print {5}
f2()

s = set([])
s2 = s.copy()
s.add(1)
print s, s2

s1 = set([3, 5])
s2 = set([1, 5])
print sorted(s1.union(s2)), sorted(s1.intersection(s2))
print sorted(s1.union(range(5, 7))), sorted(s1.intersection(range(5, 7)))
print sorted(s2.union([], [], [], [])), sorted(s2.intersection())

s = frozenset([1, 5])
d = s.difference([1], [1], [2])
print d, len(s)
print

l = []
s = set(range(5))
while s:
    l.append(s.pop())
l.sort()
print l

s = set([1])
s.discard(1)
print s
s.discard(1)
print s


s = set(range(10))
print s.difference_update(range(-3, 2), range(7, 23))
print sorted(s)


# Check set subclassing:

class MySet(set):
    pass
class MyFrozenset(frozenset):
    pass

compare_to = []
for i in xrange(10):
    compare_to.append(set(range(i)))
    compare_to.append(frozenset(range(i)))
    compare_to.append(MySet(range(i)))
    compare_to.append(MyFrozenset(range(i)))
    compare_to.append(range(i))
    compare_to.append(range(i, 10))

for s1 in set(range(5)), frozenset(range(5)):
    for s2 in compare_to:
        print type(s2), sorted(s2), s.issubset(s2), s.issuperset(s2), s == s2, s != s2, s.difference(s2), s.isdisjoint(s2), sorted(s1.union(s2)), sorted(s1.intersection(s2))

f = float('nan')
s = set([f])
print f in s, f == list(s)[0]
