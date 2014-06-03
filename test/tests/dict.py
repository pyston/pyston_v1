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
