# import functools
# partial = functools.partial

def partial(func, *args):
    def inner(*a, **kw):
        return func(*(args + a), **kw)
    return inner

def g():
    f = partial(lambda *args: args, 1, 23)
    print f("hello")
g()
f = partial(lambda *args: args, 1, 23)
print f("hello")
