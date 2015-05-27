# Objects are allowed to resurrect other objects too, I guess

class C(object):
    def __init__(self, x):
        self.x = x

    def __del__(self):
        global x
        x = self.x

x = None

def test():
    c = C([])

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

test()

# make sure to override remaining references to the weakref
# in the stack since the GC will scan the stack conservatively
fact(10)

import gc
gc.collect()

print x
