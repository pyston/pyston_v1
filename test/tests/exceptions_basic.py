
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


class MyException(Exception):
    pass

def catches(e, c):
    try:
        try:
            raise e
        except c:
            return True
    except:
        return False
for e in [Exception(), AttributeError(), MyException()]:
    for c in [Exception, AttributeError, MyException, TypeError]:
        print catches(e, c)

def f():
    try:
        raise Exception()
    except:
        print True
f()


def f11():
    print "f11"
    # old style exception syntax"

    try:
        raise KeyError, 12345
    except KeyError, e:
        print e

    try:
        raise KeyError(), 12345
    except TypeError, e:
        print e
f11()

def f12():
    try:
        raise IndexError
    except (KeyError, IndexError), e:
        print e
f12()

def f13():
    try:
        raise IndexError
    except Exception, e:
        print repr(e.message)

    try:
        raise IndexError()
    except Exception, e:
        print repr(e.message)

    try:
        raise IndexError(1)
    except Exception, e:
        print repr(e.message)
f13()

def f14():
    # Multiple non-bare except clauses:
    try:
        1/0
    except ZeroDivisionError:
        pass
    except Exception:
        pass
f14()

def test_set_state():
    exc = BaseException()
    print exc.__dict__
    attrs = {"x": 1, "y": 2}
    exc.__setstate__(attrs)
    print exc.__dict__ == attrs

test_set_state()
