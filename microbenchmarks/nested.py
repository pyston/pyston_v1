def outer():
    def inner(n):
        return n
    return inner

n = 1000000
t = 0
while n:
    n = n - 1
    t = t + outer()(n)
print t
