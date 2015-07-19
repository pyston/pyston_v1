# run_args: -n
# statcheck: noninit_count('slowpath_rearrange_args_has_starargs_no_exception') == 0

def f1(*args):
    print args

def f2(a, *args):
    print a, args

def f3(a, b, *args):
    print a, b, args

def f4(a, b, c, *args):
    print a, b, c, args

def f5(a, b, c, d, *args):
    print a, b, c, d, args

def f6(a, b, c, d, e, *args):
    print a, b, c, d, e, args

def f7():
    print 'nothing'

def f8(a):
    print a

def f9(a, b):
    print a, b

def f10(a, b, c):
    print a, b, c

def f11(a, b, c, d):
    print a, b, c, d

def f12(a, b, c, d, e):
    print a, b, c, d, e

# TODO print error messages (right now they aren't right)

def test_f1():
    print "f1(*[])"
    f1(*[])

    print "f1(*[0])"
    f1(*[0])

    print "f1(*[0, 1])"
    f1(*[0, 1])

    print "f1(*[0, 1, 2])"
    f1(*[0, 1, 2])

    print "f1(*[0, 1, 2, 3])"
    f1(*[0, 1, 2, 3])

    print "f1(0, *[])"
    f1(0, *[])

    print "f1(0, *[1])"
    f1(0, *[1])

    print "f1(0, *[1, 2])"
    f1(0, *[1, 2])

    print "f1(0, *[1, 2, 3])"
    f1(0, *[1, 2, 3])

    print "f1(0, *[1, 2, 3, 4])"
    f1(0, *[1, 2, 3, 4])

    print "f1(0, 1, *[])"
    f1(0, 1, *[])

    print "f1(0, 1, *[2])"
    f1(0, 1, *[2])

    print "f1(0, 1, *[2, 3])"
    f1(0, 1, *[2, 3])

    print "f1(0, 1, *[2, 3, 4])"
    f1(0, 1, *[2, 3, 4])

    print "f1(0, 1, *[2, 3, 4, 5])"
    f1(0, 1, *[2, 3, 4, 5])

    print "f1(0, 1, 2, *[])"
    f1(0, 1, 2, *[])

    print "f1(0, 1, 2, *[3])"
    f1(0, 1, 2, *[3])

    print "f1(0, 1, 2, *[3, 4])"
    f1(0, 1, 2, *[3, 4])

    print "f1(0, 1, 2, *[3, 4, 5])"
    f1(0, 1, 2, *[3, 4, 5])

    print "f1(0, 1, 2, *[3, 4, 5, 6])"
    f1(0, 1, 2, *[3, 4, 5, 6])

    print "f1(0, 1, 2, 3, *[])"
    f1(0, 1, 2, 3, *[])

    print "f1(0, 1, 2, 3, *[4])"
    f1(0, 1, 2, 3, *[4])

    print "f1(0, 1, 2, 3, *[4, 5])"
    f1(0, 1, 2, 3, *[4, 5])

    print "f1(0, 1, 2, 3, *[4, 5, 6])"
    f1(0, 1, 2, 3, *[4, 5, 6])

    print "f1(0, 1, 2, 3, *[4, 5, 6, 7])"
    f1(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f2():
    print "f2(*[])"
    try:
        f2(*[])
    except Exception as e:
        print "exception"

    print "f2(*[0])"
    f2(*[0])

    print "f2(*[0, 1])"
    f2(*[0, 1])

    print "f2(*[0, 1, 2])"
    f2(*[0, 1, 2])

    print "f2(*[0, 1, 2, 3])"
    f2(*[0, 1, 2, 3])

    print "f2(0, *[])"
    f2(0, *[])

    print "f2(0, *[1])"
    f2(0, *[1])

    print "f2(0, *[1, 2])"
    f2(0, *[1, 2])

    print "f2(0, *[1, 2, 3])"
    f2(0, *[1, 2, 3])

    print "f2(0, *[1, 2, 3, 4])"
    f2(0, *[1, 2, 3, 4])

    print "f2(0, 1, *[])"
    f2(0, 1, *[])

    print "f2(0, 1, *[2])"
    f2(0, 1, *[2])

    print "f2(0, 1, *[2, 3])"
    f2(0, 1, *[2, 3])

    print "f2(0, 1, *[2, 3, 4])"
    f2(0, 1, *[2, 3, 4])

    print "f2(0, 1, *[2, 3, 4, 5])"
    f2(0, 1, *[2, 3, 4, 5])

    print "f2(0, 1, 2, *[])"
    f2(0, 1, 2, *[])

    print "f2(0, 1, 2, *[3])"
    f2(0, 1, 2, *[3])

    print "f2(0, 1, 2, *[3, 4])"
    f2(0, 1, 2, *[3, 4])

    print "f2(0, 1, 2, *[3, 4, 5])"
    f2(0, 1, 2, *[3, 4, 5])

    print "f2(0, 1, 2, *[3, 4, 5, 6])"
    f2(0, 1, 2, *[3, 4, 5, 6])

    print "f2(0, 1, 2, 3, *[])"
    f2(0, 1, 2, 3, *[])

    print "f2(0, 1, 2, 3, *[4])"
    f2(0, 1, 2, 3, *[4])

    print "f2(0, 1, 2, 3, *[4, 5])"
    f2(0, 1, 2, 3, *[4, 5])

    print "f2(0, 1, 2, 3, *[4, 5, 6])"
    f2(0, 1, 2, 3, *[4, 5, 6])

    print "f2(0, 1, 2, 3, *[4, 5, 6, 7])"
    f2(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f3():
    print "f3(*[])"
    try:
        f3(*[])
    except Exception as e:
        print "exception"

    print "f3(*[0])"
    try:
        f3(*[0])
    except Exception as e:
        print "exception"

    print "f3(*[0, 1])"
    f3(*[0, 1])

    print "f3(*[0, 1, 2])"
    f3(*[0, 1, 2])

    print "f3(*[0, 1, 2, 3])"
    f3(*[0, 1, 2, 3])

    print "f3(0, *[])"
    try:
        f3(0, *[])
    except Exception as e:
        print "exception"

    print "f3(0, *[1])"
    f3(0, *[1])

    print "f3(0, *[1, 2])"
    f3(0, *[1, 2])

    print "f3(0, *[1, 2, 3])"
    f3(0, *[1, 2, 3])

    print "f3(0, *[1, 2, 3, 4])"
    f3(0, *[1, 2, 3, 4])

    print "f3(0, 1, *[])"
    f3(0, 1, *[])

    print "f3(0, 1, *[2])"
    f3(0, 1, *[2])

    print "f3(0, 1, *[2, 3])"
    f3(0, 1, *[2, 3])

    print "f3(0, 1, *[2, 3, 4])"
    f3(0, 1, *[2, 3, 4])

    print "f3(0, 1, *[2, 3, 4, 5])"
    f3(0, 1, *[2, 3, 4, 5])

    print "f3(0, 1, 2, *[])"
    f3(0, 1, 2, *[])

    print "f3(0, 1, 2, *[3])"
    f3(0, 1, 2, *[3])

    print "f3(0, 1, 2, *[3, 4])"
    f3(0, 1, 2, *[3, 4])

    print "f3(0, 1, 2, *[3, 4, 5])"
    f3(0, 1, 2, *[3, 4, 5])

    print "f3(0, 1, 2, *[3, 4, 5, 6])"
    f3(0, 1, 2, *[3, 4, 5, 6])

    print "f3(0, 1, 2, 3, *[])"
    f3(0, 1, 2, 3, *[])

    print "f3(0, 1, 2, 3, *[4])"
    f3(0, 1, 2, 3, *[4])

    print "f3(0, 1, 2, 3, *[4, 5])"
    f3(0, 1, 2, 3, *[4, 5])

    print "f3(0, 1, 2, 3, *[4, 5, 6])"
    f3(0, 1, 2, 3, *[4, 5, 6])

    print "f3(0, 1, 2, 3, *[4, 5, 6, 7])"
    f3(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f4():
    print "f4(*[])"
    try:
        f4(*[])
    except Exception as e:
        print "exception"

    print "f4(*[0])"
    try:
        f4(*[0])
    except Exception as e:
        print "exception"

    print "f4(*[0, 1])"
    try:
        f4(*[0, 1])
    except Exception as e:
        print "exception"

    print "f4(*[0, 1, 2])"
    f4(*[0, 1, 2])

    print "f4(*[0, 1, 2, 3])"
    f4(*[0, 1, 2, 3])

    print "f4(0, *[])"
    try:
        f4(0, *[])
    except Exception as e:
        print "exception"

    print "f4(0, *[1])"
    try:
        f4(0, *[1])
    except Exception as e:
        print "exception"

    print "f4(0, *[1, 2])"
    f4(0, *[1, 2])

    print "f4(0, *[1, 2, 3])"
    f4(0, *[1, 2, 3])

    print "f4(0, *[1, 2, 3, 4])"
    f4(0, *[1, 2, 3, 4])

    print "f4(0, 1, *[])"
    try:
        f4(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f4(0, 1, *[2])"
    f4(0, 1, *[2])

    print "f4(0, 1, *[2, 3])"
    f4(0, 1, *[2, 3])

    print "f4(0, 1, *[2, 3, 4])"
    f4(0, 1, *[2, 3, 4])

    print "f4(0, 1, *[2, 3, 4, 5])"
    f4(0, 1, *[2, 3, 4, 5])

    print "f4(0, 1, 2, *[])"
    f4(0, 1, 2, *[])

    print "f4(0, 1, 2, *[3])"
    f4(0, 1, 2, *[3])

    print "f4(0, 1, 2, *[3, 4])"
    f4(0, 1, 2, *[3, 4])

    print "f4(0, 1, 2, *[3, 4, 5])"
    f4(0, 1, 2, *[3, 4, 5])

    print "f4(0, 1, 2, *[3, 4, 5, 6])"
    f4(0, 1, 2, *[3, 4, 5, 6])

    print "f4(0, 1, 2, 3, *[])"
    f4(0, 1, 2, 3, *[])

    print "f4(0, 1, 2, 3, *[4])"
    f4(0, 1, 2, 3, *[4])

    print "f4(0, 1, 2, 3, *[4, 5])"
    f4(0, 1, 2, 3, *[4, 5])

    print "f4(0, 1, 2, 3, *[4, 5, 6])"
    f4(0, 1, 2, 3, *[4, 5, 6])

    print "f4(0, 1, 2, 3, *[4, 5, 6, 7])"
    f4(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f5():
    print "f5(*[])"
    try:
        f5(*[])
    except Exception as e:
        print "exception"

    print "f5(*[0])"
    try:
        f5(*[0])
    except Exception as e:
        print "exception"

    print "f5(*[0, 1])"
    try:
        f5(*[0, 1])
    except Exception as e:
        print "exception"

    print "f5(*[0, 1, 2])"
    try:
        f5(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f5(*[0, 1, 2, 3])"
    f5(*[0, 1, 2, 3])

    print "f5(0, *[])"
    try:
        f5(0, *[])
    except Exception as e:
        print "exception"

    print "f5(0, *[1])"
    try:
        f5(0, *[1])
    except Exception as e:
        print "exception"

    print "f5(0, *[1, 2])"
    try:
        f5(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f5(0, *[1, 2, 3])"
    f5(0, *[1, 2, 3])

    print "f5(0, *[1, 2, 3, 4])"
    f5(0, *[1, 2, 3, 4])

    print "f5(0, 1, *[])"
    try:
        f5(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f5(0, 1, *[2])"
    try:
        f5(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f5(0, 1, *[2, 3])"
    f5(0, 1, *[2, 3])

    print "f5(0, 1, *[2, 3, 4])"
    f5(0, 1, *[2, 3, 4])

    print "f5(0, 1, *[2, 3, 4, 5])"
    f5(0, 1, *[2, 3, 4, 5])

    print "f5(0, 1, 2, *[])"
    try:
        f5(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f5(0, 1, 2, *[3])"
    f5(0, 1, 2, *[3])

    print "f5(0, 1, 2, *[3, 4])"
    f5(0, 1, 2, *[3, 4])

    print "f5(0, 1, 2, *[3, 4, 5])"
    f5(0, 1, 2, *[3, 4, 5])

    print "f5(0, 1, 2, *[3, 4, 5, 6])"
    f5(0, 1, 2, *[3, 4, 5, 6])

    print "f5(0, 1, 2, 3, *[])"
    f5(0, 1, 2, 3, *[])

    print "f5(0, 1, 2, 3, *[4])"
    f5(0, 1, 2, 3, *[4])

    print "f5(0, 1, 2, 3, *[4, 5])"
    f5(0, 1, 2, 3, *[4, 5])

    print "f5(0, 1, 2, 3, *[4, 5, 6])"
    f5(0, 1, 2, 3, *[4, 5, 6])

    print "f5(0, 1, 2, 3, *[4, 5, 6, 7])"
    f5(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f6():
    print "f6(*[])"
    try:
        f6(*[])
    except Exception as e:
        print "exception"

    print "f6(*[0])"
    try:
        f6(*[0])
    except Exception as e:
        print "exception"

    print "f6(*[0, 1])"
    try:
        f6(*[0, 1])
    except Exception as e:
        print "exception"

    print "f6(*[0, 1, 2])"
    try:
        f6(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f6(*[0, 1, 2, 3])"
    try:
        f6(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f6(0, *[])"
    try:
        f6(0, *[])
    except Exception as e:
        print "exception"

    print "f6(0, *[1])"
    try:
        f6(0, *[1])
    except Exception as e:
        print "exception"

    print "f6(0, *[1, 2])"
    try:
        f6(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f6(0, *[1, 2, 3])"
    try:
        f6(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f6(0, *[1, 2, 3, 4])"
    f6(0, *[1, 2, 3, 4])

    print "f6(0, 1, *[])"
    try:
        f6(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f6(0, 1, *[2])"
    try:
        f6(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f6(0, 1, *[2, 3])"
    try:
        f6(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f6(0, 1, *[2, 3, 4])"
    f6(0, 1, *[2, 3, 4])

    print "f6(0, 1, *[2, 3, 4, 5])"
    f6(0, 1, *[2, 3, 4, 5])

    print "f6(0, 1, 2, *[])"
    try:
        f6(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f6(0, 1, 2, *[3])"
    try:
        f6(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f6(0, 1, 2, *[3, 4])"
    f6(0, 1, 2, *[3, 4])

    print "f6(0, 1, 2, *[3, 4, 5])"
    f6(0, 1, 2, *[3, 4, 5])

    print "f6(0, 1, 2, *[3, 4, 5, 6])"
    f6(0, 1, 2, *[3, 4, 5, 6])

    print "f6(0, 1, 2, 3, *[])"
    try:
        f6(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f6(0, 1, 2, 3, *[4])"
    f6(0, 1, 2, 3, *[4])

    print "f6(0, 1, 2, 3, *[4, 5])"
    f6(0, 1, 2, 3, *[4, 5])

    print "f6(0, 1, 2, 3, *[4, 5, 6])"
    f6(0, 1, 2, 3, *[4, 5, 6])

    print "f6(0, 1, 2, 3, *[4, 5, 6, 7])"
    f6(0, 1, 2, 3, *[4, 5, 6, 7])


def test_f7():
    print "f7(*[])"
    f7(*[])

    print "f7(*[0])"
    try:
        f7(*[0])
    except Exception as e:
        print "exception"

    print "f7(*[0, 1])"
    try:
        f7(*[0, 1])
    except Exception as e:
        print "exception"

    print "f7(*[0, 1, 2])"
    try:
        f7(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f7(*[0, 1, 2, 3])"
    try:
        f7(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f7(0, *[])"
    try:
        f7(0, *[])
    except Exception as e:
        print "exception"

    print "f7(0, *[1])"
    try:
        f7(0, *[1])
    except Exception as e:
        print "exception"

    print "f7(0, *[1, 2])"
    try:
        f7(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f7(0, *[1, 2, 3])"
    try:
        f7(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f7(0, *[1, 2, 3, 4])"
    try:
        f7(0, *[1, 2, 3, 4])
    except Exception as e:
        print "exception"

    print "f7(0, 1, *[])"
    try:
        f7(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f7(0, 1, *[2])"
    try:
        f7(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f7(0, 1, *[2, 3])"
    try:
        f7(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f7(0, 1, *[2, 3, 4])"
    try:
        f7(0, 1, *[2, 3, 4])
    except Exception as e:
        print "exception"

    print "f7(0, 1, *[2, 3, 4, 5])"
    try:
        f7(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, *[])"
    try:
        f7(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, *[3])"
    try:
        f7(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, *[3, 4])"
    try:
        f7(0, 1, 2, *[3, 4])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, *[3, 4, 5])"
    try:
        f7(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f7(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, 3, *[])"
    try:
        f7(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, 3, *[4])"
    try:
        f7(0, 1, 2, 3, *[4])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, 3, *[4, 5])"
    try:
        f7(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f7(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f7(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f7(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"


def test_f8():
    print "f8(*[])"
    try:
        f8(*[])
    except Exception as e:
        print "exception"

    print "f8(*[0])"
    f8(*[0])

    print "f8(*[0, 1])"
    try:
        f8(*[0, 1])
    except Exception as e:
        print "exception"

    print "f8(*[0, 1, 2])"
    try:
        f8(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f8(*[0, 1, 2, 3])"
    try:
        f8(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f8(0, *[])"
    f8(0, *[])

    print "f8(0, *[1])"
    try:
        f8(0, *[1])
    except Exception as e:
        print "exception"

    print "f8(0, *[1, 2])"
    try:
        f8(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f8(0, *[1, 2, 3])"
    try:
        f8(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f8(0, *[1, 2, 3, 4])"
    try:
        f8(0, *[1, 2, 3, 4])
    except Exception as e:
        print "exception"

    print "f8(0, 1, *[])"
    try:
        f8(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f8(0, 1, *[2])"
    try:
        f8(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f8(0, 1, *[2, 3])"
    try:
        f8(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f8(0, 1, *[2, 3, 4])"
    try:
        f8(0, 1, *[2, 3, 4])
    except Exception as e:
        print "exception"

    print "f8(0, 1, *[2, 3, 4, 5])"
    try:
        f8(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, *[])"
    try:
        f8(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, *[3])"
    try:
        f8(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, *[3, 4])"
    try:
        f8(0, 1, 2, *[3, 4])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, *[3, 4, 5])"
    try:
        f8(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f8(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, 3, *[])"
    try:
        f8(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, 3, *[4])"
    try:
        f8(0, 1, 2, 3, *[4])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, 3, *[4, 5])"
    try:
        f8(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f8(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f8(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f8(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"


def test_f9():
    print "f9(*[])"
    try:
        f9(*[])
    except Exception as e:
        print "exception"

    print "f9(*[0])"
    try:
        f9(*[0])
    except Exception as e:
        print "exception"

    print "f9(*[0, 1])"
    f9(*[0, 1])

    print "f9(*[0, 1, 2])"
    try:
        f9(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f9(*[0, 1, 2, 3])"
    try:
        f9(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f9(0, *[])"
    try:
        f9(0, *[])
    except Exception as e:
        print "exception"

    print "f9(0, *[1])"
    f9(0, *[1])

    print "f9(0, *[1, 2])"
    try:
        f9(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f9(0, *[1, 2, 3])"
    try:
        f9(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f9(0, *[1, 2, 3, 4])"
    try:
        f9(0, *[1, 2, 3, 4])
    except Exception as e:
        print "exception"

    print "f9(0, 1, *[])"
    f9(0, 1, *[])

    print "f9(0, 1, *[2])"
    try:
        f9(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f9(0, 1, *[2, 3])"
    try:
        f9(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f9(0, 1, *[2, 3, 4])"
    try:
        f9(0, 1, *[2, 3, 4])
    except Exception as e:
        print "exception"

    print "f9(0, 1, *[2, 3, 4, 5])"
    try:
        f9(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, *[])"
    try:
        f9(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, *[3])"
    try:
        f9(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, *[3, 4])"
    try:
        f9(0, 1, 2, *[3, 4])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, *[3, 4, 5])"
    try:
        f9(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f9(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, 3, *[])"
    try:
        f9(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, 3, *[4])"
    try:
        f9(0, 1, 2, 3, *[4])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, 3, *[4, 5])"
    try:
        f9(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f9(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f9(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f9(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"


def test_f10():
    print "f10(*[])"
    try:
        f10(*[])
    except Exception as e:
        print "exception"

    print "f10(*[0])"
    try:
        f10(*[0])
    except Exception as e:
        print "exception"

    print "f10(*[0, 1])"
    try:
        f10(*[0, 1])
    except Exception as e:
        print "exception"

    print "f10(*[0, 1, 2])"
    f10(*[0, 1, 2])

    print "f10(*[0, 1, 2, 3])"
    try:
        f10(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f10(0, *[])"
    try:
        f10(0, *[])
    except Exception as e:
        print "exception"

    print "f10(0, *[1])"
    try:
        f10(0, *[1])
    except Exception as e:
        print "exception"

    print "f10(0, *[1, 2])"
    f10(0, *[1, 2])

    print "f10(0, *[1, 2, 3])"
    try:
        f10(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f10(0, *[1, 2, 3, 4])"
    try:
        f10(0, *[1, 2, 3, 4])
    except Exception as e:
        print "exception"

    print "f10(0, 1, *[])"
    try:
        f10(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f10(0, 1, *[2])"
    f10(0, 1, *[2])

    print "f10(0, 1, *[2, 3])"
    try:
        f10(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f10(0, 1, *[2, 3, 4])"
    try:
        f10(0, 1, *[2, 3, 4])
    except Exception as e:
        print "exception"

    print "f10(0, 1, *[2, 3, 4, 5])"
    try:
        f10(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, *[])"
    f10(0, 1, 2, *[])

    print "f10(0, 1, 2, *[3])"
    try:
        f10(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, *[3, 4])"
    try:
        f10(0, 1, 2, *[3, 4])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, *[3, 4, 5])"
    try:
        f10(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f10(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, 3, *[])"
    try:
        f10(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, 3, *[4])"
    try:
        f10(0, 1, 2, 3, *[4])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, 3, *[4, 5])"
    try:
        f10(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f10(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f10(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f10(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"


def test_f11():
    print "f11(*[])"
    try:
        f11(*[])
    except Exception as e:
        print "exception"

    print "f11(*[0])"
    try:
        f11(*[0])
    except Exception as e:
        print "exception"

    print "f11(*[0, 1])"
    try:
        f11(*[0, 1])
    except Exception as e:
        print "exception"

    print "f11(*[0, 1, 2])"
    try:
        f11(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f11(*[0, 1, 2, 3])"
    f11(*[0, 1, 2, 3])

    print "f11(0, *[])"
    try:
        f11(0, *[])
    except Exception as e:
        print "exception"

    print "f11(0, *[1])"
    try:
        f11(0, *[1])
    except Exception as e:
        print "exception"

    print "f11(0, *[1, 2])"
    try:
        f11(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f11(0, *[1, 2, 3])"
    f11(0, *[1, 2, 3])

    print "f11(0, *[1, 2, 3, 4])"
    try:
        f11(0, *[1, 2, 3, 4])
    except Exception as e:
        print "exception"

    print "f11(0, 1, *[])"
    try:
        f11(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f11(0, 1, *[2])"
    try:
        f11(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f11(0, 1, *[2, 3])"
    f11(0, 1, *[2, 3])

    print "f11(0, 1, *[2, 3, 4])"
    try:
        f11(0, 1, *[2, 3, 4])
    except Exception as e:
        print "exception"

    print "f11(0, 1, *[2, 3, 4, 5])"
    try:
        f11(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, *[])"
    try:
        f11(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, *[3])"
    f11(0, 1, 2, *[3])

    print "f11(0, 1, 2, *[3, 4])"
    try:
        f11(0, 1, 2, *[3, 4])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, *[3, 4, 5])"
    try:
        f11(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f11(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, 3, *[])"
    f11(0, 1, 2, 3, *[])

    print "f11(0, 1, 2, 3, *[4])"
    try:
        f11(0, 1, 2, 3, *[4])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, 3, *[4, 5])"
    try:
        f11(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f11(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f11(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f11(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"


def test_f12():
    print "f12(*[])"
    try:
        f12(*[])
    except Exception as e:
        print "exception"

    print "f12(*[0])"
    try:
        f12(*[0])
    except Exception as e:
        print "exception"

    print "f12(*[0, 1])"
    try:
        f12(*[0, 1])
    except Exception as e:
        print "exception"

    print "f12(*[0, 1, 2])"
    try:
        f12(*[0, 1, 2])
    except Exception as e:
        print "exception"

    print "f12(*[0, 1, 2, 3])"
    try:
        f12(*[0, 1, 2, 3])
    except Exception as e:
        print "exception"

    print "f12(0, *[])"
    try:
        f12(0, *[])
    except Exception as e:
        print "exception"

    print "f12(0, *[1])"
    try:
        f12(0, *[1])
    except Exception as e:
        print "exception"

    print "f12(0, *[1, 2])"
    try:
        f12(0, *[1, 2])
    except Exception as e:
        print "exception"

    print "f12(0, *[1, 2, 3])"
    try:
        f12(0, *[1, 2, 3])
    except Exception as e:
        print "exception"

    print "f12(0, *[1, 2, 3, 4])"
    f12(0, *[1, 2, 3, 4])

    print "f12(0, 1, *[])"
    try:
        f12(0, 1, *[])
    except Exception as e:
        print "exception"

    print "f12(0, 1, *[2])"
    try:
        f12(0, 1, *[2])
    except Exception as e:
        print "exception"

    print "f12(0, 1, *[2, 3])"
    try:
        f12(0, 1, *[2, 3])
    except Exception as e:
        print "exception"

    print "f12(0, 1, *[2, 3, 4])"
    f12(0, 1, *[2, 3, 4])

    print "f12(0, 1, *[2, 3, 4, 5])"
    try:
        f12(0, 1, *[2, 3, 4, 5])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, *[])"
    try:
        f12(0, 1, 2, *[])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, *[3])"
    try:
        f12(0, 1, 2, *[3])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, *[3, 4])"
    f12(0, 1, 2, *[3, 4])

    print "f12(0, 1, 2, *[3, 4, 5])"
    try:
        f12(0, 1, 2, *[3, 4, 5])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, *[3, 4, 5, 6])"
    try:
        f12(0, 1, 2, *[3, 4, 5, 6])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, 3, *[])"
    try:
        f12(0, 1, 2, 3, *[])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, 3, *[4])"
    f12(0, 1, 2, 3, *[4])

    print "f12(0, 1, 2, 3, *[4, 5])"
    try:
        f12(0, 1, 2, 3, *[4, 5])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, 3, *[4, 5, 6])"
    try:
        f12(0, 1, 2, 3, *[4, 5, 6])
    except Exception as e:
        print "exception"

    print "f12(0, 1, 2, 3, *[4, 5, 6, 7])"
    try:
        f12(0, 1, 2, 3, *[4, 5, 6, 7])
    except Exception as e:
        print "exception"

for i in xrange(150):
    test_f1()
    test_f2()
    test_f3()
    test_f4()
    test_f5()
    test_f6()
    test_f7()
    test_f8()
    test_f9()
    test_f10()
    test_f11()
    test_f12()

try:
    import __pyston__
    __pyston__.clearStats()
except ImportError:
    pass

for i in xrange(25):
    test_f1()
    test_f2()
    test_f3()
    test_f4()
    test_f5()
    test_f6()
    test_f7()
    test_f8()
    test_f9()
    test_f10()
    test_f11()
    test_f12()
