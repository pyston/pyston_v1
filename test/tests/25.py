# Regression test

def ident(x):
    return x

y = 10
while y:
    print ident(y)
    y = y - 1

def a(x):
    print "a", x

def b(a, x):
    a(x)

b(a, 1)
b(a, 2)

