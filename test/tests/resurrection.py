# expected: fail
#  - finalization (let alone resurrection) not implemented yet

# Objects are allowed to resurrect themselves in their __del__ methods...
# Note: the behavior here will differ from cPython and maybe PyPy

x = None
running = True

class C(object):
    def __init__(self):
        self.n = 0
    def __del__(self):
        if running:
            global x
            self.n += 1
            print "__del__ #%d" % self.n
            x = self

import gc

x = C()
for i in xrange(100):
    x = None
    gc.collect()
    # print x
running = False
