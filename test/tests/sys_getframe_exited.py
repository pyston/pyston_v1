import sys
def f():
    var = 42
    return sys._getframe(0)

fr = f()
print fr.f_locals


def f():
    var = 0
    fr = sys._getframe(0)
    var += 1
    return fr
fr = f()
print fr.f_locals["var"]
