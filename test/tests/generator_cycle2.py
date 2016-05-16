# Test a generator cycle involving an unfinished generator.
# this used to leak in the interpreter
import gc
def f(z):
    l = ((lambda x, l: x**y)(z, l) for y in xrange(10))
    return l

def test():
    g = f(4)
    print g.next()
    return g

g = test()
print g.next()
gc.collect()
print gc.garbage
