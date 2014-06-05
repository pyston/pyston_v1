# expected: fail
# - keywords

def f(a, b, c):
    print a, b, c

f(1, 2, 3)
f(1, b=2, c=3)
f(1, b=2, c=3)
f(1, c=2, b=3)

f(1, b="2", c=3)
f(1, b=2, c="3")
f(1, c="2", b=3)
f(1, c=2, b="3")
