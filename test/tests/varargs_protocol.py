# expected: fail
# - not supported yet

class C(object):
    def __len__(self):
        print "__len__"
        return 2

    def __iter__(self):
        print "__iter__"
        return self

    def next(self):
        print "Next"
        raise StopIteration()

def f(a, b, c):
    print a, b, c

f(*C())
