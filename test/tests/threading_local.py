import gc
import threading

a = threading.local()
a.x = "hello world"

def f():
    a.x = "goodbye world"
    print a.x

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
