# Regression test: making sure we can deopt unboxed int methods later:

def ident(x):
    return x

x = 1
a = x.__add__
print a(2)
print ident(a)(2)
print ident(a)(ident(2))

def o():
    return 1
print 1 + o()
