def dict2str(d):
    result = ''
    for k, v in sorted(d.items()):
        if result:
            result += ', '
        result += '%s: %s' % (str(k), str(v))
    return '{%s}' % result

print dict2str({i: j for i in range(4) for j in range(4)})

def f():
    print dict2str({i: j for i in range(4) for j in range(4)})
    # print i, j
f()

def f2(x):
    print dict2str({x: i for i in [x]})
    print dict2str({i: i for i in [x]})
f2(7)

# Combine a dict comprehension with a bunch of other control-flow expressions:
def f3(x, y):
    # TODO make sure to use an 'if' in a comprehension where the if contains control flow
    print dict2str({y if i % 3 else y ** 2 + i: (i if i%2 else i/2) for i in (xrange(4 if x else 5) if y else xrange(3))})
f3(0, 0)
f3(0, 1)
f3(1, 0)
f3(1, 1)

# TODO: test on ifs

def f4():
    print dict2str({i : j for (i, j) in sorted({1:2, 3:4, 5:6, 7:8}.items())})
f4()

# The expr should not get evaluated if the if-condition fails:
def f5():
    def p(i):
        print i
        return i ** 2
    def k(i):
        print i
        return i * 4 + i

    print dict2str({k(i):p(i) for i in xrange(50) if i % 5 == 0 if i % 3 == 0})
f5()

def f6():
    print dict2str({i: j for i in xrange(4) for j in xrange(i)})
f6()

def f7():
    j = 1
    # The 'if' part of this list comprehension references j;
    # the first time through it will use the j above, but later times
    # it may-or-may-not use the j from the inner part of the listcomp.
    print dict2str({i: j for i in xrange(7) if i % 2 != j % 2 for j in xrange(i)})
    # XXX: why is this here? if we un-indent this line, python raises an exception
    f7()

def f8():
    # Checking the order of evaluation of the if conditions:

    def c1(x):
        print "c1", x
        return x % 2 == 0

    def c2(x):
        print "c2", x
        return x % 3 == 0

    print dict2str({i : i for i in xrange(20) if c1(i) if c2(i)})
f8()

def f9():
    # checking that dictcomps don't contaminate our scope like listcomps do
    print dict2str({i:j for i,j in [(1,2)]})
    try: print i
    except NameError as e: print e
    try: print j
    except NameError as e: print e

    print dict2str({1:2 for x in xrange(4) for y in xrange(5)})
    try: print x
    except NameError as e: print e
    try: print y
    except NameError as e: print e
f9()

def f10():
    x = 'for'
    y = 'eva'
    print dict2str({i:j for i in [x for x in xrange(6)] for j in [y for y in [1]]})
    print x, y
f10()

def control_flow_in_dictcomp():
    print dict2str({(i ** 2 if i > 5 else i ** 2 * -1):(i if i else -1) for i in (xrange(10) if True else []) if (i % 2 == 0 or i % 3 != 0)})
control_flow_in_dictcomp()
