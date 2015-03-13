for i in range(10):
    print i
for i in xrange(1, 15, 2):
    print i

l = [2, 3, 5, 7]
d = {}
for i in l:
    d[i-1] = i+1
for t in sorted(d.iteritems()):
    print t

for i in xrange(-5, 5):
    for j in xrange(i):
        print i, j
    else:
        print "else"

for i in xrange(-10, 10):
    if i % 3 == 0:
        continue
    print i
    if i == 3:
        break
else:
    print "else"

for i in xrange(5):
    print i
    continue
else:
    print "else", i

for i in xrange(5):
    print i
    break

def f():
    for i in xrange(5):
        if i == 10:
            return 2
    else:
        return 5

f()
