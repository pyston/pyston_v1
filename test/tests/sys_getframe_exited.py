# expected: fail
# - we don't (yet?) support looking at frame objects after
#   their frame has exited
import sys
def f():
    return sys._getframe(0)

fr = f()
print fr.f_locals
