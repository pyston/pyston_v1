# making sure we can deoptimize within a loop (line 9)

def d(x):
    return x - 1

def f(d):
    x = 10
    while x:
        x = d(x)

x = 10000
y = 0
while x:
    f(d)
    x = x - 1
print y
