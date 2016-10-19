# import ctypes
# 
# print ctypes.pythonapi.PyArg_ParseTuple(globals()

import api_test

api_test.test_attrwrapper_parse(globals())
def f():
    pass
api_test.test_attrwrapper_parse(f.__dict__)


f.a = 1
d = {'a': 1}
d2 = f.__dict__

assert d2 == d2
assert d == d2
assert d2 == d
assert not (d < d2)
assert not (d2 < d)
assert not (d > d2)
assert not (d2 > d)
assert d <= d2
assert d2 <= d
assert d >= d2
assert d2 >= d
assert not (d2 != d2)
assert not (d2 != d)
assert not (d != d2)
