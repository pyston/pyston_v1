# attr-getting resolution.

class M(type):
    def __getattribute__(self, attr):
        print "M.__getattribute__"
        return type.__getattribute__(self, attr)

    def __eq__(lhs, rhs):
        print "__eq__"
        return 0

class C(object):
    __metaclass__ = M

    def __getattribute__(self, attr):
        print "C.__getattribute__", attr
        if attr == "n":
            return 1
        return object.__getattribute__(self, attr)

    def __getattr__(self, attr):
        print "C.__getattr__", attr
        if attr == "m":
            return 2
        return object.__getattr__(self, attr)

c = C()
print c.n
print c.m
c.__getattribute__
C.__getattribute__

print C == C
print c == c
