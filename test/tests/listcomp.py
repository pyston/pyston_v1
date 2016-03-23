print [(i, j) for i in range(4) for j in range(4)]

def f():
    print [(i, j) for i in range(4) for j in range(4)]
    print i, j
f()


# Combine a list comprehension with a bunch of other control-flow expressions:
def f(x, y):
    # TODO make sure to use an 'if' in a comprehension where the if contains control flow
    print [(i if i%2 else i/2) for i in (xrange(4 if x else 5) if y else xrange(3))]
f(0, 0)
f(0, 1)
f(1, 0)
f(1, 1)

# TODO: test on ifs

def f():
    print [(i, j) for (i, j) in sorted({1:2, 3:4, 5:6, 7:8}.items())]
f()

# The expr should not get evaluated if the if-condition fails:
def f():
    def p(i):
        print i
        return i ** 2

    print [p(i) for i in xrange(50) if i % 5 == 0 if i % 3 == 0]
f()

def f():
    print [(i, j) for i in xrange(4) for j in xrange(i)]
f()

def f():
    j = 1
    # The 'if' part of this list comprehension references j;
    # the first time through it will use the j above, but later times
    # it may-or-may-not use the j from the inner part of the listcomp.
    print [(i, j) for i in xrange(7) if i%2 != j%2 for j in xrange(i)]
f()

def f():
    # Checking the order of evaluation of the if conditions:

    def c1(x):
        print "c1", x
        return x % 2 == 0

    def c2(x):
        print "c2", x
        return x % 3 == 0

    print [i for i in xrange(20) if c1(i) if c2(i)]
f()

def control_flow_in_listcomp():
    print [(i if i else -1) for i in (xrange(10) if True else []) if (i % 2 == 0 or i % 3 != 0)]
control_flow_in_listcomp()
