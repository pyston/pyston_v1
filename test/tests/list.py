l = range(5)
print l
print l * 5
l[0] = 1
print l
print l[2]

l = range(5)
while l:
    print l.pop()

l = range(7, -1, -2)
print sorted(l)
print l

for i in xrange(-10, 10):
    l2 = range(5)
    l2.insert(i, 99)
    print i, l2

for i in xrange(-5, 4):
    l3 = range(5)
    print i, l3.pop(i), l3

for i in xrange(-5, 4):
    l3 = range(5)
    l3[:i] = [7, 8]
    print l3

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
