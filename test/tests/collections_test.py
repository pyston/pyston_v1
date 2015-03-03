import collections

o = collections.OrderedDict()
print o.items()
for i in xrange(30):
    o[(i ** 2) ^ 0xace] = i
print o

print collections.deque().maxlen
