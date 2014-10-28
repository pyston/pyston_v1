def f1():
    l = []
    for i in xrange(5):
        l.append(i ** 2)
        print sorted(locals().items())
f1()

def f():
    total = 0
    i = 1 or ''
    while i < 20:
        i = i + 1
        j = 2
        while j * j <= i:
            if i % j == 0:
                break
            j = j + 1
            print sorted(locals().items())
        else:
            total = total + i
    print total
f()

def f3():
    """Testing unboxed values"""
    x = 1.0
    y = 1
    z = 123456789123456789
    s = "hello world"
    t = (1.0, "asdf")
    print sorted(locals().items())
f3()

def f4():
    """testing synthetic 'is_defined' variables"""
    if 0:
        x = 1
    else:
        y = 2
    print sorted(locals().items())
# f4()
