class C(object):
    def __init__(self, a):
        print a

C(1)
C(a=2)
C(*(3,))
C(**{'a':4})


# Regression test: make sure we pass these through correctly:
class C(object):
    def __init__(self, k=None):
        print k

def f(*args, **kw):
    print args, kw
    return C(*args, **kw)

f(k=1)
