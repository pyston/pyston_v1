import sys

def generate(lst):
    for x in lst: yield x

def f():
    g = generate([1])
    print g.next()
    try:
        g.throw(ZeroDivisionError)
    except ZeroDivisionError as e:
        print e
        # check that we have a traceback
        print sys.exc_info()[2] is None
    else:
        print "shouldn't happen"
f()

def f2():
    g = generate([1])
    print g.next()
    try:
        g.throw(ZeroDivisionError(2))
        print 'hello'
    except ZeroDivisionError as e:
        print e
        # check that we have a traceback
        print sys.exc_info()[2] is None
    else:
        print "shouldn't happen"
f2()

def f3():
    g = generate([1,2])
    print g.next()
    try:
        g.throw(ZeroDivisionError, 2, None)
    except ZeroDivisionError as e:
        print e
        cls, val, tb = sys.exc_info()
        print cls
        print val
        print isinstance(val, ZeroDivisionError)
        print tb is None
    else:
        print "shouldn't happen"
f3()
