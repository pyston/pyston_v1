# Regression test: make sure we can handle variables that are only defined
# on excluded parts of an osr compilation
def f():
    if True:
        for i in xrange(20000):
            pass
    else:
        a = 1
f()

def f2():
    if True:
        for i in xrange(20000):
            pass
    else:
        a = 1

    if False:
        print a
f2()
