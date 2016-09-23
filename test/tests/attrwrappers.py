# import ctypes
# 
# print ctypes.pythonapi.PyArg_ParseTuple(globals()

import api_test

api_test.test_attrwrapper_parse(globals())
def f():
    pass
api_test.test_attrwrapper_parse(f.__dict__)
