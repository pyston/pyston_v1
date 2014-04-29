# expected: fail
#  - finalization (let alone resurrection) not implemented yet

# Objects are allowed to resurrect other objects too, I guess

class C(object):
    def __init__(self, x):
        self.x = x

    def __del__(self):
        global x
        x = self.x

x = None
c = C([])
del c

import gc
gc.collect()

print x
