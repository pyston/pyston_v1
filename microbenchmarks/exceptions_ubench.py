def f():
    # Try to eliminate as much non-exception stuff as possible:
    from __builtin__ import Exception
    e = Exception()

    for i in xrange(100000):
        try:
            raise e
        except Exception:
            pass
f()
