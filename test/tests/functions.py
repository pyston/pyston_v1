
def f():
    print "f"
    return 2

print f.__call__()

def g():
    print "g"
    return 3

print type(f).__call__(f)
print type(f).__call__(g)

print f.__name__, f.func_name
f.__name__ = "New name"
print f.__name__, f.func_name
f.func_name = "f"
print f.__name__, f.func_name

print bool(f)
