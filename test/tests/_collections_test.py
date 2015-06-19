import _collections

d = _collections.deque()
print d
d.append(1)
d.appendleft(2)
d.append(3)
print d

print
print type(iter(d))
for i in d:
    print i

while d:
    print d.popleft()

d = _collections.defaultdict()
print str(d), repr(d)
