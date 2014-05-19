def wrap(f, n):
    if n:
        wrap(f, n-1)
    return f()

def throws():
    raise AttributeError

for i in xrange(10000):
    try:
        wrap(throws, 500)
    except AttributeError:
        pass
