
class MyList(list):
    pass

l = MyList(range(5))
print type(l)
l.foo = 1
print l.foo

print l

print len(MyList.__new__(MyList))
l[:] = l[:]
print l

print [1,2,3] == MyList((1,2,3,4))
print [1,2,3] != MyList((1,2,3,4))

print [1,2,3,4] > MyList((1,2,3))
print [1,2,3,4] < MyList((1,2,3))

print [1,2,3] > MyList((1,2,3,4))
print [1,2,3] < MyList((1,2,3,4))

print [1,2,3] >= MyList((1,2,3))
print [1,2,3] <= MyList((1,2,3))

print MyList((1,2,3)) == MyList((1,2,3,4))
print MyList((1,2,3)) != MyList((1,2,3,4))

print MyList((1,2,3,4)) > MyList((1,2,3))
print MyList((1,2,3,4)) < MyList((1,2,3))

print MyList((1,2,3)) > MyList((1,2,3,4))
print MyList((1,2,3)) < MyList((1,2,3,4))

print MyList((1,2,3)) >= MyList((1,2,3))
print MyList((1,2,3)) <= MyList((1,2,3))

print type(MyList((1, 2, 3)) * 1)

print [1] + MyList([3])

class ListWithInit(list):
    def __init__(self, *args, **kwargs):
        print "ListWithInit.__init__", args, kwargs

l = ListWithInit(1, 2, 3, a=5)
l.a = 1
l.b = 2
# Adapted from the sqlalchemy test:
import pickle
l2 = pickle.loads(pickle.dumps(l))
print l == l2
assert l.__dict__ == l2.__dict__, (l.__dict__, l2.__dict__)

# Regression test:
def f(l):
    l *= 1
for i in xrange(3000):
    f(l)
