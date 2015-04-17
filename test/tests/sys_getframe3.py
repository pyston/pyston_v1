import sys

def f():
    fr = sys._getframe(0)

    print fr.f_lineno
    print fr.f_lineno
    print sorted(fr.f_locals.keys())
    a = 1
    print sorted(fr.f_locals.keys())
f()

assert sys._getframe(0) is sys._getframe(0)

def f2():
    f1 = sys._getframe(0)

    # trigger osr:
    for i in xrange(20000):
        pass
    assert f1 is sys._getframe(0)
f2()
