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

class DictWithMissing(dict):
    def __missing__(self, key):
        print key
        return key

d = DictWithMissing()
print d[5]
