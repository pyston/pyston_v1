class C(object):
    def __setattr__(self, attr, value):
        print attr, value
        if attr.startswith("c"):
            print "yum"
        else:
            object.__setattr__(self, attr, value)

c = C()
c.a = 1
c.b = 2
c.c = 3
print sorted(c.__dict__.items())
