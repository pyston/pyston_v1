# Make sure that allocating large strings works.
# (We've had issues with this due to string memory not being GC'd or tracked by the GC

s = "." * 1000000 # 1MB

for i in xrange(1000):
    print i, len(s + '!')
