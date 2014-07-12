# You can put arbitrary stuff in class definitions, which end up being added as class attributes

class C(object):
    n = 10
    a = 1
    b = 2
    x = 100
    y = 101
    while n > 0:
        t = a + b
        a = b
        b = t
        n = n - 1

        if b == 20:
            break

    if b == 13:
        x = "hello"
    else:
        y = "world"

    class S(object):
        pass

    [123]
print C.__module__

class D(object):
    x = 1
    # Note: this fails in Python 3:
    l = [x for y in range(1)]

def p(o):
    print o.a
    print o.b
    print o.t
    print o.n
    print o.x
    print o.y
    print o.S

p(C)
p(C())
