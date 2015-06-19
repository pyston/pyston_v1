
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
