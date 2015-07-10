# This is the same as incremental_tb_bjit.py but tests the baseline JIT.
try:
    import __pyston__
    __pyston__.setOption("REOPT_THRESHOLD_INTERPRETER", 1)
except:
    pass
import traceback
import sys
def f():
    a, b, c = sys.exc_info()
    raise a, b, c

et0, ev0, tb0 = None, None, None

try:
    1/0
except:
    pass

for i in xrange(10):
    try:
        f()
    except:
        et0, ev0, tb0 = sys.exc_info()

print "******** 0", ''.join(traceback.format_exception(et0, ev0, tb0))


et1, ev1, tb1 = None, None, None
et2, ev2, tb2 = None, None, None

def f1():
    raise

def f2():
    f1()

def f21():
    raise Exception()

def f3():
    try:
        f21()
    except:
        global et1, tv1, tb1
        et1, tv1, tb1 = sys.exc_info()
        f2()


try:
    f3()
except:
    et2, tv2, tb2 = sys.exc_info()

print "******** 1", ''.join(traceback.format_exception(et1, ev1, tb1))
print "******** 2", ''.join(traceback.format_exception(et2, ev2, tb2))

print et1 is et2
print ev1 is ev2
print tb1 is tb2
