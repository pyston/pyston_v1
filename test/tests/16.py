# Deoptimization test:

def main(z):
    def ident(x):
        return x

    def f1(x):
        return x
    def f2(x):
        return "x"

    f1 = ident(f1)
    f2 = ident(f2)

    x = ident(z)
    y = 10
    while y:
        x = x + y
        y = y - 1

        print f1(y), f2(y)
main(10)

