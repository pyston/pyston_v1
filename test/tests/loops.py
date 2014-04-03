def f():
    for i in xrange(5):
        for j in xrange(5):
            print i, j
            break
        break
f()
