# Simple test that doesn't actually examine with functionality, just ensures that it's
# minimally handled:

class C(object):
    def __init__(self, n):
        self.n = n

    def __enter__(self):
        print "__enter__", self.n
        return self.n

    def __exit__(self, type, val, tb):
        print "__exit__", type, val, tb

def new_exit(self, type, val, tb):
    print "new exit!", self, type, val, tb

def bad_exit(type, val, tb):
    print "bad exit!"

c = C(1)
with c as n:
    print n
print n

# With targets can be arbitrary l values:
l = [0]
with C(2) as l[0]:
    pass
print l

c = C(3)
with c as n:
    C.__exit__ = new_exit # this shouldn't have any effect
    c.__exit__ = bad_exit
    print n
print n
