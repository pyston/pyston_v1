d = {}
for i in xrange(1000):
    d[i] = i ** 2

for k, v in sorted(d.items()):
    print k, v
