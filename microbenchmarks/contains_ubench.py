def f():
    S = set("abc")
    c = "b"
    for i in xrange(10000000):
        c in S
f()
