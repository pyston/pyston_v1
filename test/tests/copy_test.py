import copy
class C(object):
    pass

c = C()
c.a = 1
c.b = (["str", 1], 2L)
print sorted(c.__dict__.items())
cc = copy.deepcopy(c)
del c.a
print sorted(cc.__dict__.items())

