def f():
    # Try to eliminate as much non-exception stuff as possible:
    from __builtin__ import Exception
    e = Exception()

    for i in xrange(20000):
        try:
            raise e
        except Exception:
            pass

def recurse(n):
    if n:
        recurse(n - 1)
    else:
        f()
recurse(100)
