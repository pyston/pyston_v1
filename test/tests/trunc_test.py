import math

def type_trunc(type, arg):
    try:
        print type.__trunc__(arg)
    except TypeError as e:
        print e

type_trunc(float, 5.25)
type_trunc(float, 5)
type_trunc(float, 5L)
type_trunc(float, "5")

type_trunc(int, 5.25)
type_trunc(int, 5)
type_trunc(int, 5L)
type_trunc(int, "5")

type_trunc(long, 5.25)
type_trunc(long, 5)
type_trunc(long, 5L)
type_trunc(long, "5")

class Test:
    def __trunc__(self):
        print "made it"
        return 5

t = Test()
print math.trunc(t)

class TruncReturnsNonInt:
    def __trunc__(self):
        print "made it"
        return "hi"

t2 = TruncReturnsNonInt()
print math.trunc(t2)
