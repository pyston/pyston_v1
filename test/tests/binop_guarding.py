class C(object):
    def __add__(self, rhs):
        if rhs == 50:
            return NotImplemented
        return 0

    __eq__ = __add__

def f():
    c = C()
    for i in xrange(100):
        try:
            print i, c + i
        except TypeError as e:
            print e
f()

def f2():
    c = C()
    for i in xrange(100):
        try:
            print i, c == i
        except TypeError as e:
            print e
f2()
