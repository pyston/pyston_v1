# attr perf test: functions are still the only
# things that support setattr, so use those

def p(x, p):
    p.n = p.n + x.a
p.n = 0

def s(x, n):
    x.a = n
    x.b = "hello world"

n = 1000000
while n:
    n = n - 1
    def f():
        pass
    s(f, n)
    p(f, p)
print p.n
