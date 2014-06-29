d = {}
print d.get(1)
print d.setdefault(2)
print d
d[2] = 5
print d.pop(2)
print d.pop(2, None)
print d.pop(2, None)

print range(5)
print range(5, 10)

print list(xrange(5))
print list(xrange(5, 10))

def f1(f):
    def f(x=f):
        return x

    print f()
    print f(2)
f1(1)
f1(3)

def f2():
    x = 0
    def f(x, y=1, z=x):
        print x, y, z
    f(1)
    f(2, 3)
    x = 2
    f(4, 5, 6)
f2()

def f3():
    def f(x, l=[]):
        l.append(x)
        print l

    f(0)
    f(1)
    f(2)
    f(4, [])
    f(5, [])
f3()
