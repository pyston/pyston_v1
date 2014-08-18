# - descriptors

#  Descriptors get processed when fetched as part of a dunder lookup

class D(object):
    def __init__(self, n):
        self.n = n
    def __get__(self, obj, cls):
        print "__get__()", obj is None, self.n
        def desc(*args):
            print "desc()", len(args)
            return self.n
        return desc

    def __call__(self):
        print "D.call"
        return self.n

class C(object):
    __hash__ = D(1)
    __add__ = D(2)

c = C()
print C.__hash__()
print c.__hash__()
print hash(c)
print c + c
