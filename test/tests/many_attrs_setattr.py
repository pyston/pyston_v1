# expected: fail
# - setattr() not supported

class C(object):
    pass

c = C()

n = 0
while n < 100000:
    setattr(c, "a" + str(n), n)
    n = n + 1
    if n % 1000 == 0:
        print n

def f(o):
    print o.a1
    print o.a10
    print o.a100
    print o.a1000
    print o.a10000
    print o.a99999

f(c)
f(c)
