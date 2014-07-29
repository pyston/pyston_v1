l = 2L
print l
print type(l)

t = 1L
for i in xrange(150):
    t *= l
    print t, repr(t)

print 1L / 5L
print -1L / 5L
print 1L / -5L
print -1L / -5L
