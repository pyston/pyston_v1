# getitem

def f(l):
    n = 0
    while n < len(l):
        print l[n]
        n = n + 1

f([2, 3, 5, 7])
f("hello world")

class C(object):
    def __getitem__(self, sl):
        print "orig getitem"
        return sl

def gi(sl):
    print "new getitem"
    return -1
c = C()
c.__getitem__ = gi
print c[1]

try:
    print 1[1]
except TypeError, e:
    print e
