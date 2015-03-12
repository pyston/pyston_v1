import collections

o = collections.OrderedDict()
print o.items()
for i in xrange(30):
    o[(i ** 2) ^ 0xace] = i
print o
print o.copy()

print collections.deque().maxlen

d = collections.defaultdict(lambda: [])
print d[1]
d[2].append(3)
print sorted(d.items())
