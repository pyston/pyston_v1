# Regression test: make sure that args and kw get properly treated as potentially-saved-in-closure

def f1(*args):
    def inner():
        return args
    return inner

print f1()()
print f1(1, 2, 3, "a")()

def f2(**kw):
    def inner():
        return kw
    return inner

print f2()()
print f2(a=1)()
