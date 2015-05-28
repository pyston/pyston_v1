import gc
import threading

a = threading.local()
a.x = "hello world"

class CustomThreadingLocal(threading.local):
    n = 0
    def __init__(self):
        print "__init__", self.n
        self.a = self.n
        self.n += 1
        print self.a, self.n
print CustomThreadingLocal().a
c = CustomThreadingLocal()
print c.a

def f():
    print
    a.x = "goodbye world"
    print a.x
    print c.a
    print CustomThreadingLocal().a

def test():
    thread = threading.Thread(target=f)
    thread.start()
    thread.join()

    print a.x
    print getattr(a, "doesnt_exist", 5)

for i in xrange(10):
    test()
    gc.collect()

print a.x
a.__setattr__('x', 5)
print a.x
print sorted(a.__dict__.items())

del a.x
print sorted(a.__dict__.items())
