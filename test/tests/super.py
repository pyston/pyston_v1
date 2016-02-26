class B(object):
    def __new__(cls, arg1):
        print "B.__new__", arg1
        o = super(B, cls).__new__(cls)
        o.arg1 = arg1
        return o

    def f(self):
        print "B.f()"

class C(B):
    def __new__(cls, arg1, arg2):
        print "C.__new__", arg2
        o = super(C, cls).__new__(cls, arg1)
        o.arg2 = arg2
        print super(C, cls), super(C, o)
        return o

    def f(self):
        print "C.f()"
        super(C, self).f()
        print super(C, self).__thisclass__
        try:
            super(C, self).does_not_exist
        except AttributeError as e:
            print e

c = C(1, 2)
print c.arg1
print c.arg2
c.f()

try:
    super(1)
except Exception, e:
    print e

try:
    super(int, [])
except Exception, e:
    print e
