# A harder deopt test: there can be temporaries that aren't in the symbol table that need to be transferred
# to the deopt version

def p(x, y):
    print x, y
def i1(x):
    return x
def i2(x):
    return "x"
p(i1(1), i2(1))

def ident(x):
    return 2
print 1 + ident(1)
def m():
    def ident(x):
        return 2
    print 1 + ident(1)
m()

def f(n, f):
    if n <= 0:
        return 0
    return 1 + f(n-1, f) + 1
print f(10, f)

def f2(n, f):
    if n <= 2:
        return n
    return f(n-1, f) + f(n-2, f)
print f2(10, f2)
