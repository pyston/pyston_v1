# should_error

# The use of c makes sure a closure gets passed through all 4 functions.
# The use of a in g makes sure that a is in f's closure.
# The a in j should refer to the a in h, thus throwing an exception since
# it is undefined (that is, it should *not* access the a from f even
# though it access via the closure).

def f():
    c = 0
    a = 0
    def g():
        print c
        print a
        def h():
            print c
            def j():
                print c
                print a

            j()
            a = 1
        h()
    g()
f()
