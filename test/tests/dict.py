d = {2:2}
d[1] = 1
print d
print d[1]

d = {}
for i in xrange(10):
    d[i] = i ** 2
print sorted(d.items())
print sorted(d.values())
print sorted(d.keys())
print sorted(d.iteritems())
print sorted(d.itervalues())
print sorted(d.iterkeys())

l = []
for i in d:
    l.append(i)
print sorted(l)

print d.pop(5, 5)
print sorted(d.items())
print d.pop(5, 5)
print sorted(d.items())

print d.pop(4)
try:
    d.pop(5)
    assert 0
except KeyError, e:
    print e.message
    print "ok"
print sorted(d.items())

print d.get(4)
print d.get(4, 5)
print d.get(3, 5)

print sorted(d.items())
print d.setdefault(11, 9)
print sorted(d.items())
print d.setdefault(11, 10)
print sorted(d.items())

print dict()
d = dict()
d[1] = 2
print d

print 1 in {}
print 1 in {1:1}
print 'a' in {}
print 'a' in {'a': 1}

print 'a' in dict()
print 'a' in dict(a=1)
print d

print dict(**dict(a=1, b=2))

# __new__
print sorted(dict([['a', 1], ['b', 2]]))
print sorted(dict([('a', 1), ('b', 2)]))
print sorted(dict((['a', 1], ['b', 2])))
print sorted(dict((('a', 1), ('b', 2))))
print sorted(dict((('a', 1), ['b', 2])))

print sorted(dict([['a', 1], ['b', 2]])) == sorted(dict((['a', 1], ['b', 2])))
print sorted(dict([('a', 1), ('b', 2)])) == sorted(dict((('a', 1), ['b', 2])))
print sorted(dict((('a', 1), ('b', 2)), b=3)) == sorted(dict((['a', 1], ('b', 3))))

try:
    print dict([1,2], [2,3])
except TypeError, e:
    print e

try:
    print dict([(1,2), 42])
except TypeError, e:
    print e

try:
    # invalid tuple len
    print dict([(10,20), (1,2,3)])
except ValueError, e:
    print e

try:
    # invalid list len
    print dict([[10,20], [1,2,3]])
except ValueError, e:
    print e

d = {i:i**2 for i in xrange(10)}
print sorted(d.items())
del d[2]
print d.__delitem__(4)
print sorted(d.items())

try:
    del d[2]
except KeyError, e:
    print e

d = {1:[2]}
d2 = d.copy()
print d2, d
d2[1].append(1)
print d2, d
d2[1] = 1
print d2, d

# __init__
d = {}
print d.__init__((('a', 1), ('b', 2)), b=3)
print sorted(d.items())

# clear
d = {1:2, 'a': 'b'}
d.clear()

print d

d = {}
d.clear()
print d

# fromkeys

d = {1:2, 3:4}

print sorted(d.fromkeys([1,2]).items())
print sorted(d.fromkeys([]).items())
print sorted(d.fromkeys([3,4], 5).items())

try:
    print d.fromkeys()
    assert 0
except TypeError, e:
    print 'ok'

# has_key

d = {1:2, 3:4, 'a': 5}
print d.has_key(1)
print d.has_key(42)
print d.has_key('a')
print d.has_key('b')

# popitem - actual order is implementation defined

d = {1:2, 3:4, 'a': 5}

l = []
l.append(d.popitem())
l.append(d.popitem())
l.append(d.popitem())

print sorted(l)

try:
    d.popitem()
    assert 0
except KeyError, e:
    print 'ok'
