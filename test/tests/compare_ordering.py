# - we have an extra evaluation of __nonzero__ at the end

class A(object):
    def __init__(self, n):
        print "init", n
        self.n = n
    def __lt__(self, rhs):
        print "lt", self.n, rhs.n
        return self
    def __nonzero__(self):
        print "nonzero", self.n
        return True
    def __repr__(self):
        return "<A (%d)>" % self.n

print A(1) < A(2) < A(3)
