class C(object):
    def __eq__(self, rhs):
        print "eq", type(self), type(rhs)
        return False

    def __lt__(self, rhs):
        print "lt", type(self), type(rhs)
        return False

    def __gt__(self, rhs):
        print "gt", type(self), type(rhs)
        return True

class D(C):
    def __cmp__(self, rhs):
        print "cmp", type(self), type(rhs)
        return 0

l = [C(), D()]
for lhs in l:
    for rhs in l:
        r = cmp(lhs, rhs)
        print type(lhs), type(rhs), r
