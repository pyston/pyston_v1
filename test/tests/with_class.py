def f():
    # originally this exposed a bug in our irgen phase, so even `with None`
    # failed here; the bug happened before actual execution. Just to test more
    # things, though, we use an actual contextmanager here.
    with open('/dev/null'):
        class C(object):
            print 'hello'

f()
