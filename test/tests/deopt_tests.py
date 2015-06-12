# skip-if: '-O' in EXTRA_JIT_ARGS
# expected: statfail
# statcheck: 4 <= noninit_count('num_deopt') < 50
# statcheck: 1 <= stats["num_osr_exits"] <= 2

try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("REOPT_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("SPECULATION_THRESHOLD", 10)
except ImportError:
    pass

def main():
    var_in_closure1 = 0
    
    def f(o):
        print "starting f"

        try:
            print o.a
            if o.b:
                raise Exception('')
        except Exception, e:
            print o.c
            print e
        print o.d
        print sorted(locals().items())

        print var_in_closure1
        var_in_closure2 = 1
        def g():
            print var_in_closure2
        g()

        print "Done"

    class C(object):
        def __repr__(self):
            return "<C>"

    c = C()
    c.a = 1
    c.b = 0
    c.c = 3
    c.d = 4

    for i in xrange(2000):
        print i

        if i == 500:
            c.a = []

        if i == 600:
            c.b = 1

        if i == 700:
            c.c = []

        if i == 800:
            c.b = 0
            c.d = 1.0

        f(c)

    # Regression test reduced from subprocess.py:
    import types
    def f2(self, args):
        if isinstance(args, types.StringTypes):
            pass

        try:
            self.pid
        except:
            pass

    c = C()
    c.pid = 1
    for i in xrange(2000):
        f2(c, None)

        if i == 1500:
            c.pid = 1.0
main()


# This test tries to OSR up to the highest tier and then deopt from there:
def test_deopt_from_maximal(C):
    c = C()
    c.a = 1
    def f(c):
        n = 1000000
        while n:
            c.a
            n -= 1
    f(c)
    c.a = None
    f(c)

# Test for both old-style and new-style classes:
class C:
    pass
class D(object):
    pass
test_deopt_from_maximal(C)
test_deopt_from_maximal(D)
