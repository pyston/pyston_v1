# skip-if: '-O' in EXTRA_JIT_ARGS or '-n'  in EXTRA_JIT_ARGS
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
    class C(object):
        def __repr__(self):
            return "<C>"

    def f_gen(o):
        print "starting f_gen, yielding:"

        yield 8

        try:
            print o.a
            if o.b:
                raise Exception('')
        except Exception, e:
            print o.c
            print e
        print o.d
        print sorted(locals().items())

        print "yielding again:"
        yield 9

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

        g = f_gen(c)
        print 'yielded(1):', g.next()
        print 'yielded(2):', g.next()

main()
