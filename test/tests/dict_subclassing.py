class MyDict(dict):
    pass

d = MyDict()
d[1] = 2
print d.keys()
print d.values()
print d.items()
print list(d.iterkeys())
print list(d.itervalues())
print list(d.iteritems())
