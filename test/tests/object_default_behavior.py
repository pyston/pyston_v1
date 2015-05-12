class C(object):
    def __delattr__(self, attr):
        print "delattr", attr
        if attr.startswith("c"):
            print "yum"
        else:
            object.__delattr__(self, attr)

    def __setattr__(self, attr, value):
        print attr, value
        if attr.startswith("c"):
            print "yum"
        else:
            object.__setattr__(self, attr, value)

    def __getattribute__(self, attr):
        print "getattribute", attr
        if attr.startswith("c"):
            return "yum"
        return object.__getattribute__(self, attr)

c = C()
c.a = 1
c.b = 2
c.c = 3
print sorted(c.__dict__.items())
print c.a, c.b, c.c

del c.a
del c.b
del c.c
print sorted(c.__dict__.items())

v = 1
print object.__hash__(v) == object.__hash__(v)
