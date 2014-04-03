def p(x):
    print x.a
    print x.b

def s(x, n):
    x.a = n
    x.b = "hello world"

n = 100
while n:
    n = n - 1
    def f():
        pass
    s(f, n)
    p(f)
