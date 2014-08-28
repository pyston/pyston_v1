# Test the behavior of tuple unpacking in the face of exceptions being thrown at certain points.
# - If an exception gets thrown in the "unpack to a given size" part, none of the targets get set
# - If setting a target throws an exception, then the previous targets had been set, but not the future ones

class C(object):
    def __init__(self, n):
        self.n = n

    def __getitem__(self, i):
        print "__getitem__", i
        if i < self.n:
            return i
        raise IndexError

    def __len__(self):
        print "len"
        return 2

def f1():
    print "f1"
    try:
        x, y = C(1)
    except ValueError, e:
        print e

    try:
        print x
    except NameError, e:
        print e
    try:
        print y
    except NameError, e:
        print e
f1()

def f2():
    print "f2"
    class D(object):
         pass

    d = D()
    def f():
        print "f"
        return d

    try:
        f().x, f().y = C(1)
    except ValueError, e:
        print e

    print hasattr(d, "x")
    print hasattr(d, "y")
f2()

def f3():
    print "f3"
    class D(object):
         pass

    d = D()
    def f(should_raise):
        print "f", should_raise
        if should_raise:
            1/0
        return d

    try:
        f(False).x, f(True).y, f(False).z = (1, 2, 3)
    except ZeroDivisionError, e:
        print e

    print hasattr(d, "x")
    print hasattr(d, "y")
    print hasattr(d, "z")
f3()

def f4():
    print "f4"
    c = C(10000)

    try:
        x, y = c
    except ValueError, e:
        print e
f4()

def f5():
    print "f5"
    def f():
        1/0

    try:
        x, f().x, y = (1, 2, 3)
    except ZeroDivisionError, e:
        print e

    try:
        print x
    except NameError, e:
        print e

    try:
        print y
    except NameError, e:
        print e
f5()
