class C(object):
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
