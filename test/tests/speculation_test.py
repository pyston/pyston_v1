# Not really that robust of a test, but checks to make sure that
# different constructs handle the PARTIAL state in irgen.

def f(n):
    return n

def f2():
    (), [], f(1)

for i in xrange(100000):
    f2()
