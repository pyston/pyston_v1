from ctypes import *
libc = CDLL("libc.so.6")
qsort = libc.qsort
qsort.restype = None
CMPFUNC = CFUNCTYPE(c_int, POINTER(c_int), POINTER(c_int))
def py_cmp_func(a, b):
    1/0
cmp_func = CMPFUNC(py_cmp_func)

IntArray3 = c_int * 3
ia = IntArray3(1, 2, 3)
qsort(ia, len(ia), sizeof(c_int), cmp_func)
print "finished"
