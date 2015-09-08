# Make sure that callattrs handle exceptions (including
# different exception styles) correctly.
class C(object):
    def __getattr__(self, attr):
        raise ValueError()

def f():
    c = C()
    for i in xrange(10000):
        try:
            c.foo()
        except ValueError:
            pass
f()
