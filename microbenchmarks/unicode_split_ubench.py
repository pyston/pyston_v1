def f():
    u = "a b c d"
    u2 = u" "
    for i in xrange(4000000):
        u.split(u2)
f()
