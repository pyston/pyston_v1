# expected: fail

class C(object):
    def __init__(self, n):
        self.n = n

    def __lt__(self, rhs):
        print "lt"
        return self.n < rhs.n

    def __le__(self, rhs):
        print "le"
        return self.n <= rhs.n

def f(n):
    print "f(%d)" % n
    return C(n)

# f(3) shouldn't get called:
f(1) <= f(2) < f(1) < f(3)
