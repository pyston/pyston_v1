import copy
class C(object):
    pass

c = C()
c.a = 1
c.b = (["str", 1], 2L)
print sorted(c.__dict__.items())
cc = copy.deepcopy(c)
copy_dict = copy.deepcopy(c.__dict__)
del c.a
print sorted(cc.__dict__.items())
print sorted(copy_dict.items())
