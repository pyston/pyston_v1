class C(object):
    def __add__(self, rhs):
        print "__add__", rhs
        return 1
    def __radd__(self, lhs):
        print "__radd__", lhs
        return 2

class D(object):
    def __repr__(self):
        return "<D object>"

def f():
    print 1.0 + 1
    print 1 + 1.0
    print D() + C()
    print C() + D()
    print 1 + C()
    print C() + 1
# Call it twice to test patching
f()
f()

print 1.0 / 2
