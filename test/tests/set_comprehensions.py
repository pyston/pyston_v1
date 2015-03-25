# For now, just check that we can parse set comprehensions even if we can't run them:

def f():
    print sorted({i%3 for i in xrange(5)})
