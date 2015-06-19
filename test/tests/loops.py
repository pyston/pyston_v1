def f():
    for i in xrange(5):
        for j in xrange(5):
            print i, j
            break
        break
f()

def f2(x):
    while x:
        return 1
    else:
        return 2
print f2(1)

