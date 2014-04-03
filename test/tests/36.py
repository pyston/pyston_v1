# Another variation on a theme:

def d(x):
    return x - 1

def f(d):
    x = 10
    while x:
        x = d(x)

x = 100005
while x:
    f(d)
    x = x - 1

f(d)
if 0:
    pass
d
