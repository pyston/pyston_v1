import test

print test
test.store([])
print test.load()

class C(object):
    def __init__(self):
        self.x = self.y = self.z = self.w = self.k = self.a = self.b = self.c = 1

for i in xrange(100000):
    C()

print "This will break"
print test.load()
