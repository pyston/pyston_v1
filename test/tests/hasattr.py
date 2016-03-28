a = 'test'
assert hasattr(a, "__str__")
assert hasattr(a, "dupa") == False

class C(object):
    pass
c = C()

assert not hasattr(c, "a")
c.a = 1
print getattr(c, "a")
assert hasattr(c, "a")

assert not hasattr(c, "b")
print setattr(c, "b", 5)
assert hasattr(c, "b")
print c.b
