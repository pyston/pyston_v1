# Regression test: make sure that we can handle various kinds of written type errors.
# ex if we can prove that an expression would throw an error due to type issues, we should
# still compile and run the function just fine.
#
# We definitely try to support this, but it can be tricky sometimes since the different phases
# (mainly, type analysis and irgen) have to agree on the way they deal with the error.  If they
# don't, irgen will crash after it detects a mismatch with type analysis.

def f(p):
    if not "opaque branch condition":
        k = p[1]
        print k

        l = len(p)
        print l

        t = p + 1
        print t

# Run in a loop to trigger OSR:
for i in xrange(20):
    f(None)
print "done"


try:
    print 1.0 & 1.0
except TypeError, e:
    print e

try:
    print 1.0[0]
except TypeError, e:
    print e



def f2():
    if 0:
        1 in l
        l = []

for i in xrange(100):
    f2()


# make sure we don't abort when calling a type which is not callable
def f(call):
    i = 1
    f = 1.0
    l = 1l
    s = "str"
    t = (1,)
    
    if call:
       i()
       f()
       l()
       s()
       t()
try:
    for i in range(10000):
        f(i == 9999)
except TypeError, e:
    print e


# make sure we don't abort when calling getitem
def f(error):
    i = 1
    f = 1.0
    l = 1l
    
    if error:
       i[:]
       f[:]
       l[:]
       i[1]
       f[1]
       l[1]
       i[1:2]
       f[1:2]
       l[1:2]

try:
    for i in range(10000):
        f(i == 9999)
except TypeError as e:
    print e
