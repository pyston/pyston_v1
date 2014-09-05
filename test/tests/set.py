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
