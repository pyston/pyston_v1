# expected: fail
#  - finalization not implemented yet

# Finalizers should be called before any objects are deallocated
# Note: the behavior here will differ from cPython and maybe PyPy

finalizers_run = []
class C(object):
    def __init__(self, n):
        self.n = n
        self.x = None

    def __del__(self):
        finalizers_run.append((self.n, self.x.n if self.x else None))

def f():
    x1 = C(1)
    x2 = C(2)
    x1.x = x2
    x2.x = x1
f()

finalizers_run.sort()
print finalizers_run
