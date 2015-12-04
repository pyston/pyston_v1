import sys
def f():
    return sys._getframe(0)

fr = f()
print fr.f_locals
