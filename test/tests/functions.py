
def f():
    print "f"
    return 2

print f.__call__()

def g():
    print "g"
    return 3

print type(f).__call__(f)
print type(f).__call__(g)

print bool(f)
