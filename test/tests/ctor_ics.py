# run_args: -n
# statcheck: noninit_count('slowpath_runtimecall') < 40
# statcheck: noninit_count('slowpath_typecall') < 30
def f():
    class B(object):
        def __init__(self):
            pass

    for i in xrange(10000):
        b = B()

    class C(object):
        pass

    for i in xrange(10000):
        c = C()

    class D(object):
        def __init__(self, a, b, c, d, e):
            pass

    class E(object):
        def __init__(self, a, b, c):
            pass

    for i in xrange(10000):
        e = E(i, i, i)
        d = D(i, i, i, i, i)



    # Can't define a custom new yet, since can't call object.__new__
    # class F(object):
        # def __new__(self, a, b, c):
            # pass

    for i in xrange(1000):
        # F(1, 2, 3)

        # Use this instead:
        xrange(1, 2, 3)
f()
