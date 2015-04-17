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

NT = collections.namedtuple("NT", ["field1", "field2"])
print NT.__name__, NT
n = NT(1, "hi")
print n.field1, n.field2, len(n), list(n), n[0], n[-1]
print n
