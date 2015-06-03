def f():
    S = set("abc")
    c = "b"
    for i in xrange(5000000):
        c in S
f()
