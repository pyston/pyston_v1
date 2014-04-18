# Augassigns are somewhat odd because they encode both a use and a def of a variable.
# So for the sake of determining if a scope sets a variable, they count as a def, but
# for the sake of whether or not a variable is live, they count as a use.


def f0(a, b):
    a += b
    return a
print f0(1, 2)

def f1(a, b):
    a += b
    if 1:
        pass
    return a
print f1(1, 2)

def f2(a, b):
    if 1:
        pass
    a += b
    return a
print f2(1, 2)

def f3(a, b):
    if 1:
        pass
    a += b
    if 1:
        pass
    return a
print f3(1, 2)

def f4():
    lists = [[], [], []]

    for i in xrange(3):
        [l for l in lists[:i+1]][-1] += [i]

    print lists
    print l
f4()

def f5():
    # Not very sensical, but this works:
    [x for x in xrange(5)][0] += x
    print x
f5()

def f6():
    x = -100
    x += [x for x in [50, 51, 52]][0]
    print x
    # Prints "-50"
f6()

def f7():
    global c
    class C(object):
        pass
    c = C()
    c.x = 0

    def inner1():
        global c
        print "inner1"
        c.x = 25
        return c

    def inner2():
        global c
        print "inner2"
        c.x = 50
        return 1

    inner1().x += inner2()
    print c.x # prints "26"
f7()

def f8():
    global l
    l = [0]

    def inner1():
        global l
        print "inner1"
        l[0] = 25
        return l

    def inner2():
        global l
        print "inner2"
        l[0] = 50
        return 1

    inner1()[0] += inner2()
    print l[0] # prints "26"
f8()

x = 9
def f9():
    global x

    def inner():
        global x
        x = 5
        return 1

    x += inner()
    print x
f9()

def f10():
    # This should error: the lhs is evaluated first
    x += [x for x in xrange(5)][0]
    print x
f10()
