def f(x):
    if x:
        pass
    else:
        x = 1
    return x
def ident(o):
    return o

ident = ident(ident)
f = ident(f)
print f(-1)
print f(0)
print f(1)
