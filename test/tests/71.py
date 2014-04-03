# __getattribute__, __getattr__

class C1(object):
    def __getattr__(self, name):
        print "__getattr__", name
        return name

class C2(object):
    def __getattribute__(self, name):
        print "__getattribute__", name
        return name

c1 = C1()
c1.a = 1
print c1.a
print c1.b

c2 = C2()
c2.a = 2
print c2.a
print c2.b
