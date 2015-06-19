# skip-if: '-O' in EXTRA_JIT_ARGS
# expected: statfail
# statcheck: 4 <= noninit_count('num_deopt') < 50
# statcheck: 1 <= stats["num_osr_exits"] <= 2

try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("REOPT_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("OSR_THRESHOLD_INTERPRETER", 50)
    __pyston__.setOption("REOPT_THRESHOLD_INTERPRETER", 50)
    __pyston__.setOption("SPECULATION_THRESHOLD", 10)
except ImportError:
    pass

# This test makes sure that the boxedLocals survive a deopt.
# TODO Write a test case to make sure exc_info survives the deopt.

def f_with_name_scoping(o):
    print "starting f"

    exec "k = 5"
    l = 6

    try:
        print o.a
        if o.b:
            raise Exception('')
    except Exception, e:
        print o.c
        print e
    print o.d
    print sorted(locals().items())

    print "k =", k
    print l

    print "Done"

def main():
    class C(object):
        def __repr__(self):
            return "<C>"

    c = C()
    c.a = 1
    c.b = 0
    c.c = 3
    c.d = 4

    for i in xrange(300):
        print i

        if i == 60:
            c.a = []

        if i == 120:
            c.b = 1

        if i == 180:
            c.c = []

        if i == 240:
            c.b = 0
            c.d = 1.0

        f_with_name_scoping(c)

main()
