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

s = s1 = set()
s |= MySet(range(2))
print sorted(s), sorted(s1)
s &= MySet(range(1))
print sorted(s), sorted(s1)
s ^= MySet(range(4))
print sorted(s), sorted(s1)
s -= MySet(range(3))
print sorted(s), sorted(s1)

try:
    set() | range(5)
    assert 0
except TypeError as e:
    print e

compare_to = []
for i in xrange(10):
    compare_to.append(set(range(i)))
    compare_to.append(frozenset(range(i)))
    compare_to.append(MySet(range(i)))
    compare_to.append(MyFrozenset(range(i)))
    compare_to.append(range(i))
    compare_to.append(range(i, 10))
    compare_to.append([0, 0, 1, 1])

for s1 in set(range(5)), frozenset(range(5)):
    for s2 in compare_to:
        print type(s2), sorted(s2), s1.issubset(s2), s1.issuperset(s2), sorted(s1.difference(s2)), s1.isdisjoint(s2), sorted(s1.union(s2)), sorted(s1.intersection(s2)), sorted(s1.symmetric_difference(s2))
        print s1 == s2, s1 != s2
        try:
            print s1 < s2, s1 <= s2, s1 > s2, s1 >= s2
        except Exception as e:
            print e
f = float('nan')
s = set([f])
print f in s, f == list(s)[0]

for fn in (set.intersection_update, set.difference_update, set.symmetric_difference_update, set.__sub__,
            set.__or__, set.__xor__, set.__and__):
    s1 = set([3, 5])
    s2 = set([1, 5])
    r = fn(s1, s2)
    if r:
        print r,
    print sorted(s1), sorted(s2)

def test_set_creation(base):
    print "Testing with base =", base
    # set.__new__ should not iterate through the argument.
    # sqlalchemy overrides init and expects to be able to do the iteration there.
    def g():
        for i in xrange(5):
            print "iterating", i
            yield i

    print "Calling __new__:"
    s = base.__new__(base, g())
    print "Calling __init__:"
    s.__init__(g())

    print "Trying subclassing"
    class MySet(base):
        def __new__(cls, g):
            print "starting new"
            r = base.__new__(cls, g)
            print "ending new"
            return r
        def __init__(self, g):
            print "starting init"
            print list(g)


    print MySet(g())
test_set_creation(set)
test_set_creation(frozenset)

set(**{})
try:
    set(**dict(a=1))
except TypeError:
    print "TypeError"


class MySet(set):
    def __new__(cls, *args, **kwargs):
        return set.__new__(cls, *args)

try:
    MySet(a=1)
except TypeError as e:
    print(e.message)


class SetSubclassWithKeywordArgs(set):
    def __init__(self, iterable=[], newarg=None):
        set.__init__(self, iterable)

SetSubclassWithKeywordArgs(newarg=1)

try:
    frozenset(a=1)
except TypeError as e:
    print(e.message)


class MyFrozenSet(frozenset):
    def __new__(cls, *args, **kwargs):
        return frozenset.__new__(cls, *args)

MyFrozenSet(a=1)


class FrozensetSubclassWithKeywordArgs(frozenset):
    def __init__(self, iterable=[], newarg=None):
        frozenset.__init__(self, iterable)

FrozensetSubclassWithKeywordArgs(newarg=1)

print(set() in frozenset([frozenset()]))


class MySet(set):
    def __hash__(self):
        print("calling __hash__")
        return id(self)

print("Ready")
foo = MySet()
a = set()
a.add(foo)
print(a.remove(foo))
print(foo in set())
