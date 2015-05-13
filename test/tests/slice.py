class Indexable(object):
    def __getitem__(self, idx):
        print "called getitem on object", idx

    def __delitem__(self, idx):
        print "called delitem on object", idx

    def __setitem__(self, idx, item):
        print "called setitem on object", idx

class Sliceable(object):
    def __getslice__(self, start, stop):
        print "called getslice on object", start, stop

    def __delslice__(self, start, stop):
        print "called delslice on object", start, stop

    def __setslice__(self, start, stop, item):
        print "called setslice on object", start, stop

class Both(object):
    def __getitem__(self, idx):
        print "called getitem on object", idx

    def __delitem__(self, idx):
        print "called delitem on object", idx

    def __setitem__(self, idx, item):
        print "called setitem on object", idx

    def __getslice__(self, start, stop):
        print "called getslice on object", start, stop

    def __delslice__(self, start, stop):
        print "called delslice on object", start, stop

    def __setslice__(self, start, stop, item):
        print "called setslice on object", start, stop

indexable = Indexable()
sliceable = Sliceable()
both = Both()
numbers = range(10)

# Can use index and slice notation for object with only getitem
indexable[0]
indexable[:10]
indexable[11:]
indexable[:]
indexable[3:8]
indexable[slice(1,2)]
indexable[slice(1,12,2)]

indexable[0] = 32
indexable[:] = xrange(2)
indexable[3:8] = xrange(2)
indexable[slice(1,12,2)] = xrange(2)

del indexable[0]
del indexable[:]
del indexable[3:8]
del indexable[slice(1,12,2)]

# Can't use index notation or pass in a slice for objects with only getslice
try:
    sliceable[0]
except TypeError:
    print "no index notation with index"

try:
    sliceable[1:10:2]
except TypeError:
    print "need getitem to support variable-sized steps"

sliceable[:10]
sliceable[11:]
sliceable[:]
sliceable[3:8]

# Make sure the right function gets called when both are present
both[0]
both[:]
both[3:8]
both[::2] # this should call __getitem__ since __getslice__ doesn't support steps

both[0] = xrange(2)
both[:] = xrange(2)
both[3:8] = xrange(2)
both[::2] = xrange(2)

del both[0]
del both[:]
del both[3:8]
del both [::2]

# Number lists should have the set/get/del|item/slice functions
print numbers[0]
print numbers[:]
print numbers[1:9]
numbers[0] = 42
numbers[7:8] = xrange(8)
print numbers
del numbers[0]
del numbers[5:6]
print numbers

# Number lists should support negative indices
print numbers[-1]
print numbers[-1:-1]
print numbers[:-2]
print numbers[-2:]

# String support slicing
print "abcd"[2]
print "abcd"[:2]
print "abcd"[2:]
print "abcd"[1:3]

# Other
class C(object):
    def __getitem__(self, idx):
        print idx
c = C()
c[1]
c[1:2]

sl = slice(1, 2)
print sl, sl.start, sl.stop, sl.step

sl = slice([])
print sl

sl = slice(1, 2, "hello")
print sl

C()[:,:]
C()[1:2,3:4]
C()[1:2:3,3:4:5]

