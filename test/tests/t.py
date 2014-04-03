def f():
    return range(10)

for i in xrange(1000000):
    f()

print "This will break"
