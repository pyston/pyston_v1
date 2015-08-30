d = dict(x=1, y=0)
exec """
def g():
    global y
    y += x
""" in d

def f():
    g = d['g']
    for i in xrange(1000000):
        g()
        g()
        g()
        g()
        g()
        g()
        g()
        g()
        g()
        g()
        d['y'] += i
f()
print d['y']
