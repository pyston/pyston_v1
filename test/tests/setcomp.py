# this file is adapted from dictcomp.py
def set2str(s):
    # set isn't guaranteed to keep things in sorted order (although in CPython it does AFAICT)
    # https://docs.python.org/2/library/stdtypes.html#types-set
    return '{%s}' % ', '.join(repr(x) for x in sorted(s))

print set2str({i for i in xrange(4)})
print set2str({(i,j) for i in xrange(4) for j in xrange(4)})

def f():
    print set2str({i: j for i in range(4) for j in range(4)})
f()

def f2(x):
    print set2str({(x, i) for i in [x]})
    print set2str({i for i in [x]})
f2(7)

# Combine a set comprehension with a bunch of other control-flow expressions:
def f3(x, y):
    print set2str({(y if i % 3 else y ** 2 + i, i if i%2 else i/2) for i in (xrange(4 if x else 5) if y else xrange(3))})
f3(0, 0)
f3(0, 1)
f3(1, 0)
f3(1, 1)

def f4():
    print set2str({(i, j) for i, j in sorted({1:2, 3:4, 5:6, 7:8}.items())})
f4()

# The expr should not get evaluated if the if-condition fails:
def f5():
    def p(i):
        print i
        return i ** 2
    def k(i):
        print i
        return i * 4 + i

    print set2str({(k(i), p(i)) for i in xrange(50) if i % 5 == 0 if i % 3 == 0})
f5()

def f6():
    print set2str({(i, j) for i in xrange(4) for j in xrange(i)})
f6()

def f8():
    # Checking the order of evaluation of the if conditions:
    def c1(x):
        print "c1", x
        return x % 2 == 0

    def c2(x):
        print "c2", x
        return x % 3 == 0

    print set2str({i for i in xrange(20) if c1(i) if c2(i)})
f8()

def f9():
    # checking that setcomps don't contaminate our scope like listcomps do
    print set2str({i for i in [1]})
    try: print i
    except NameError as e: print e

    print set2str({1 for x in xrange(4) for y in xrange(5)})
    try: print x
    except NameError as e: print e
    try: print y
    except NameError as e: print e
f9()

def f10():
    x = 'for'
    y = 'eva'
    print set2str({(i,j) for i in [x for x in xrange(6)] for j in [y for y in xrange(3)]})
    print x, y
f10()

def control_flow_in_setcomp():
    print set2str({(i ** 2 if i > 5 else i ** 2 * -1, i if i else -1) for i in (xrange(10) if True else []) if (i % 2 == 0 or i % 3 != 0)})
control_flow_in_setcomp()

