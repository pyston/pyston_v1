# Regression test:
# If we need to invalidate an IC, and there is a stack frame in that IC,
# we have to be careful to not clear the DecrefInfo of that IC in case
# an exception traverses that stack frame.

# In this example, g() has an IC to f() that will get invalidated when
# f() tiers up.

def h():
    def f(n):
        if n > 1000:
            raise Exception()
    def g(n):
        f(n)

    for i in xrange(10000):
        try:
            g(i)
        except:
            pass
h()
