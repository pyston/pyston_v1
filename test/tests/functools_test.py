import functools

f = functools.partial(lambda *args: args, 1, 23)
print f("hello")
print f("world", 5)


def foo(x=1, y=2):
    print ' inside foo:', x, y
    return
def wrapper(f):
    @functools.wraps(f)
    def decorated(x=2, y=3):
        f(x, y)
    return decorated
for f in [foo, wrapper(foo)]:
    print f.__name__
    f()
    f(3)

