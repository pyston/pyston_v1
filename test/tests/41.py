# Harder function-setattr tests:
def f(n):
    def inner():
        pass
    inner.n = n
    return inner

f1 = f(1)
f2 = f(2)
print f1.n
print f2.n

def a():
    pass

def i(x):
    return x

if i(1) != 0:
    a.x = 1
print a.x

def b():
    pass
b.x = 1
print b.x
if i(0) != 0:
    print b.x
