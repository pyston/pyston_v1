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
    # This should error: the lhs is evaluated first
    x += [x for x in xrange(5)][0]
    print x
f6()
