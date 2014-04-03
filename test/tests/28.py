# patching test: change type arguments

def c(f, p):
    return f(p()) * 1

def pi():
    return 1
def ps():
    return "hello world"

def ident(x):
    return x

print c(ident, pi)
print c(ident, ps)
