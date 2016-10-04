import ctypes

def f():
    pass

ctypes.pythonapi.PyFrame_New.restype = ctypes.py_object
ctypes.pythonapi.PyThreadState_Get.restype = ctypes.c_void_p

f = ctypes.pythonapi.PyFrame_New(
        ctypes.c_void_p(ctypes.pythonapi.PyThreadState_Get()),
        ctypes.py_object(f.func_code),
        ctypes.py_object({'globals': True}),
        ctypes.c_long(0)
    )

print f.f_locals

