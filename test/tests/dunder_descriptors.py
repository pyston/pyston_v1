# expected: fail
# - descriptors

#  Descriptors get processed when fetched as part of a dunder lookup

class D(object):
    def __get__(self, obj, cls):
        print "__get__()", obj is None
        def desc():
            print "desc()"
            return 1
        return desc

    def __call__(self):
        print "D.call"
        return 2

class C(object):
    __hash__ = D()

c = C()
print C.__hash__()
print c.__hash__()
print hash(c)
