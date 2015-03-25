import inspect

def f1(a, b=2, *args, **kw):
    pass
def f2():
    pass

print inspect.getargspec(f1)
print inspect.getargspec(f2)

