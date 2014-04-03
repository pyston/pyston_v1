# Basic class functionality test

class C(object):
    def __init__(self, n):
        print "C.__init__"
        self.n = n

    def f(self, m):
        n = self.n
        self.n = m
        return n + m

    def __add__(self, rhs):
        print "C.__add__"
        return self.n + rhs

print C.__name__

c = C(1)
print c.f(2)
print c.f(3)
print c + 4
c = C(5)
print c.f(6)
print c.f(7)
print c + 8
