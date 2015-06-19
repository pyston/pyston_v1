try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 50)
except ImportError:
    pass

def f1(x):
    exec """
for i in xrange(x):
    pass
print x
"""

f1(200)

def f3():
    exec """
def f2(x):
    def inner():
        return x
    return inner
"""

    g = f2(200)
    for i in xrange(200):
        g()
    print g()
f3()
