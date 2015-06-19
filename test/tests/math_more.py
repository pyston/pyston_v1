import math
try:
    print math.sqrt(-1)
except ValueError, e:
    print e

s = math.sqrt
for i in xrange(5):
    print s(1.0)
    try:
        print s(-1)
    except ValueError, e:
        print e
