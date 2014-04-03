# Simple refcounting test: make sure the temporary (result of f()) doesn't get freed until after g returns

def f():
    return "hi"

def g(s):
    print s

print g(f())
