d = {2:2}
d[1] = 1
print d
print d[1], d[1L], d[1.0], d[True]

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

print sorted(dict.fromkeys([1,2]).items())
print sorted(dict.fromkeys([]).items())
print sorted(dict.fromkeys([3,4], 5).items())

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

d = {}
d.update({1:2, 3:4})
print sorted(d.items())
print sorted(dict(d).items())

class CustomMapping(object):
    def __init__(self):
        self.n = 0

    def keys(self):
        print "keys()"
        return [1, 3, 7]

    def __getitem__(self, key):
        print key
        self.n += 1
        return self.n

print sorted(dict(CustomMapping()).items())
cm = CustomMapping()
def custom_keys():
    print "custom_keys()"
    return [2, 4, 2]
cm.keys = custom_keys
print sorted(dict(cm).items())

d = {}
d.update({'c':3}, a=1, b=2)
print sorted(d.items())

# viewkeys / viewvalues / viewitems

d = {}
keys = d.keys()
viewkeys = d.viewkeys()

print list(d.viewkeys())
print list(d.viewvalues())
print list(d.viewitems())
print 'keys of d: ', keys
print 'viewkeys of d: ', list(viewkeys)

d['a'] = 1

print list(d.viewkeys())
print list(d.viewvalues())
print list(d.viewitems())
print 'keys of d: ', keys
print 'viewkeys of d: ', list(viewkeys)

print {} == {}
d1 = {}
d2 = {}
for i in xrange(6):
    d1[i] = 5 - i
    d2[5 - i] = i
    print d1 == d2, d1 != d2


d = dict([(i, i**2) for i in xrange(10)])
i = d.iteritems()
l = []
while True:
    try:
        l.append(i.next())
    except StopIteration:
        break
print sorted(l)

#recursive printing test
d = dict()
d['two'] = d
print d


# dict() will try to access the "keys" attribute, but it should swallow all exceptions
class MyObj(object):
    def __iter__(self):
        print "iter!"
        return [(1, 2)].__iter__()

    def __getattr__(self, attr):
        print "getattr", attr
        1/0

print dict(MyObj())
