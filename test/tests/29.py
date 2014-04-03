# Repatching test: more arguments

def p(a, b, c, d, e, f):
    print a, b, c, d, e, f

y = 10
while y:
    x = 5
    if y == 2:
        x = "hello world"
    p(1, y, 2, x, 4, 5)
    y = y - 1

