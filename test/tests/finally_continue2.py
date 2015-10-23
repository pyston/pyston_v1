# Similar to finally_continue.py, but this is allowable

for i in xrange(10):
    try:
        pass
    finally:
        for i in xrange(10):
            continue
        while False:
            continue
