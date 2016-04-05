# Objects are allowed to resurrect other objects too, I guess
from testing_helpers import test_gc

class C(object):
    def __init__(self, x):
        self.x = x

    def __del__(self):
        global x
        x = self.x

x = None

def test():
    c = C([])

test_gc(test)

print x
