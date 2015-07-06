from testing_helpers import test_gc

class C(object):
    def __del__(self):
        print "C del"

class D(C):
    def __del__(self):
        print "D del"

class E(C):
    def __del__(self):
        print "E del"

class F(D, E):
    def __del__(self):
        print "F del"

class G(D, E):
    pass

class H(C):
    pass

class I(H, E):
    pass

def scopeC():
    c = C()
def scopeD():
    d = D()
def scopeE():
    e = E()
def scopeF():
    f = F()
def scopeG():
    g = G()
def scopeH():
    h = H()
def scopeI():
    i = I()

test_gc(scopeC)
test_gc(scopeD)
test_gc(scopeE)
test_gc(scopeF)
test_gc(scopeG)
test_gc(scopeH)
test_gc(scopeI)
