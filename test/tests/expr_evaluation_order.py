# The function should get evaluated before the arguments
# This is especially tricky if the evaluation of the function
# expression can change the behavior of the argument expressions.

def f1():
    global g1
    def g1(x):
        return x
    def inner(x):
        return x
    return inner

print f1()(g1(1))

def f2(y):
    global g2
    def g2(x):
        return x
    return y

print g2(f2(2))
