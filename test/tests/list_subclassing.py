
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
