import gc

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

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

def foo():
    c = C()
    d = D()
    e = E()
    f = F()
    g = G()
    h = H()
    i = I()

foo()

# override remaining references on the stack
fact(10)

gc.collect()
gc.collect()
gc.collect()
