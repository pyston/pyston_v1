def f():
    pass
f.a = 1
f.b = 2

i = iter(f.__dict__)
del f
print sorted(list(i))
