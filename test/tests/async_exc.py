import ctypes
import threading

try:
    ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_long(threading.currentThread().ident), ctypes.py_object(NotImplementedError))

    for i in xrange(10000):
        pass
    assert 0, "didn't throw expected exception"
except NotImplementedError:
    pass

tid = threading.currentThread().ident

def f():
    import time
    time.sleep(0.01)
    ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_long(tid), ctypes.py_object(NotImplementedError))

t = threading.Thread(target=f)
t.start()

try:
    while True:
        pass
except NotImplementedError:
    pass

print "done"
