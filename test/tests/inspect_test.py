import inspect

def f1(a, b=2, *args, **kw):
    pass
def f2():
    pass

class C(object):
    def __init__(self):
        pass

class D(object):
    pass

print inspect.getargspec(f1)
print inspect.getargspec(f2)
print inspect.getargspec(C.__init__)
print inspect.getargspec(C().__init__)
try:
    print inspect.getargspec(D.__init__)
except Exception as e:
    print type(e)

def G():
    yield 1
print inspect.isgenerator(f1)
print inspect.isgenerator(G)
print inspect.isgenerator(G())

