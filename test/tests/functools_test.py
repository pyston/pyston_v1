import functools

f = functools.partial(lambda *args: args, 1, 23)
print f("hello")
print f("world", 5)
