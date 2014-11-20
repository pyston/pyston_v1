# expected: fail

def f((a,b)):
    print a,b

f(range(2))
f((1, 2))

