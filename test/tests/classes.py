class C(object):
    pass
class D:
    pass
for l in [type, set, int, list, tuple, C, D]:
    print l, l.__module__

print type.__dict__['__module__'].__get__(set, None)
print type(type.__dict__['__module__'].__get__(None, set))
