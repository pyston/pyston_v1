def f():
    s = "hello world!" * 500
    count = 0
    for i in xrange(10000):
        for c in s:
            if c == '!':
                count += 1
    print count
f()
