# expected: fail
# - real iteration protocol is unsupported (no exceptions yet)

class C(object):
    def __iter__(self):
        print "orig iter"
        return [1,2,3].__iter__()

    def next(self):
        print "next?"

def newiter():
    print "new iter"
    return [9,8,7].__iter__()

# This shouldn't matter:
iter = None

# this should hit the original iter function:
for i in C():
    print i

class C(object):
    def __iter__(self):
        return self

    def next(self):
        print "next"
        raise StopIteration()

def newnext():
    print "newnext"
    raise StopIteration()
c = C()
c.next = newnext

for i in c: # should hit the old next
    print i
