# If __new__ returns a non-subclass, should not call init
# Otherwise, call the return type's __init__, not the original type's!

class C(object):
    def __new__(cls, make_cls):
        print "C.new", make_cls.__name__
        return object.__new__(make_cls)

    def __init__(self, *args):
        print "C.init", args

class D(C):
    def __new__(cls):
        print "D.new"

    def __init__(self, *args):
        print "D.init", args

class E(object):
    pass

# Route the calls through this helper so they all come from the same place:
def make(T):
    C(T)

make(C)
make(D)
make(E)

