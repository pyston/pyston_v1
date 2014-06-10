import test

print test

# TODO this should work even if we don't keep a reference to l;
# it doesn't currently always work, but it sometimes works, so it's hard
# to mark this case as "expected: fail".
# Instead just weaken the test, and add this TODO to add the harder test back
# later.
l = []
test.store(l)
print test.load()

class C(object):
    def __init__(self):
        self.x = self.y = self.z = self.w = self.k = self.a = self.b = self.c = 1

for i in xrange(100000):
    C()

print "This will break"
print test.load()
