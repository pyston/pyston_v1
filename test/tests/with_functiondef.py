def f():
    # this exposes a bug in our irgen phase, so even `with None` bugs out here;
    # the bug happens before actual execution. Just to test more things, though,
    # we use an actual contextmanager here.
    with open('/dev/null'):
        def foo():
            pass

f()
