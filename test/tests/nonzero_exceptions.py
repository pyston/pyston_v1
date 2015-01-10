class MyException(Exception):
    pass

class C(object):
    def __init__(self, x):
        self.x = x
    def __nonzero__(self):
        raise MyException(self.x)

# Make sure that we can handle nonzero() throwing an exception wherever it occurs:
try:
    print C(1) and 1
except TypeError, e:
    print e

try:
    print C(2) or 1
except TypeError, e:
    print e

try:
    if C(3):
        pass
except TypeError, e:
    print e

try:
    while C(4):
        pass
except TypeError, e:
    print e

try:
    assert C(5)
except TypeError, e:
    print e

try:
    print [1 for i in range(5) if C(6)]
except TypeError, e:
    print e

try:
    print list(1 for i in range(5) if C(7))
except TypeError, e:
    print e

try:
    print 1 if C(8) else 0
except TypeError, e:
    print e




class M(type):
    def __instancecheck__(self, instance):
        print "instancecheck", instance
        return C(9)

    def __subclasscheck__(self, sub):
        print "subclasscheck", sub
        return C(10)

class F(Exception):
    __metaclass__ = M

# We should check the type of exception against F using __subclasscheck__, but it's going throw an exception
# trying to evaluate __subclasscheck__.
# But that new error should get suppressed and the original exception not caught (by that block)
try:
    1/0
except F:
    print "shouldn't get here"
except ZeroDivisionError:
    print "ok"

# But if the exception gets thrown at a slightly different point in the
# exception-handler-filtering, the new exception propagates!
try:
    try:
        1/0
    except C() or 1:
        print "shouldn't get here"
    print "shouldn't get here 2"
except TypeError, e:
    print e


class E(object):
    def __lt__(self, rhs):
        return C("false")

# This is ok because it doesn't evaluate the result of the comparison:
print E() < 1
try:
    # This is not ok because it will!
    print E() < 1 < 1
except TypeError, e:
    print e
