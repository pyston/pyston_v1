# From PEP 255: "A generator cannot be resumed while it is actively running"

def g():
    i = me.next()
    yield i
me = g()
try:
    me.next()
except ValueError, e:
    # Should be: "generator already executing"
    print e
