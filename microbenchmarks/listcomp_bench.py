def f():
    def p(i):
        print i
        return i ** 2

    print [p(i) for i in xrange(200000000) if i % 12345 == 0 if i % 301 == 0]
f()
