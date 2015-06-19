import sys
i = sys.maxint

print "maxint:", repr(i)
print "maxint + 1:", repr(i+1)
print "maxint * 1:", repr(i*1)
print "maxint * 2", repr(i*2)
print "maxint - -1:", repr(i - (-1))
print "-maxint:", repr(-i)
j = (-i-1)
print "-maxint-1 [aka minint]:", repr(j)
print "minint + 1:", repr(j+1)
print "minint - 1:", repr(j-1)
print "-minint:", repr(-j)
print "minint * -1:", repr(j * (-1))
print "minint / -1:", repr(j / (-1))

print "2 ** 100:", 2 ** 100
