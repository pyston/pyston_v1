
def f(x):
    print x
    try:
        if x == 2:
            raise AttributeError()
        assert x
    except AssertionError:
        print "is assert"
    except:
        print "not an assert"
    else:
        print "no exception"

f(0)
f(1)
f(2)

# You can set attributes on exception objects:
e = Exception()
e.n = 1
print e.n


try:
    1/0
except ZeroDivisionError, e:
    print e.message
    print str(e), repr(e)
    print e
