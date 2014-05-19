# expected: fail
# - exceptions

class TestException(Exception):
    pass

class ExpectationFailedException(Exception):
    pass

class ExpectedException(object):
    def __init__(self, excs):
        if isinstance(excs, BaseException):
            excs = (excs,)
        self.excs = excs

    def __enter__(self):
        pass

    def __exit__(self, type, val, tback):
        if not val:
            raise ExpectationFailedException("Didn't raise any exception")
        if not isinstance(val, self.excs):
            raise ExpectationFailedException("Raised %s instead of %s" % (val, self.excs))
        print "Caught", type.__name__
        return True
expected_exception = ExpectedException

# Test the expected_exception manager:
with expected_exception(Exception):
    raise Exception()

try:
    with expected_exception(Exception):
        pass
    raise Exception("shouldn't get here")
except ExpectationFailedException:
    print "good"

# The inner one will fail, which the outer one should catch:
with expected_exception(ExpectationFailedException):
    with expected_exception(Exception):
        pass


def throw(x):
    try:
        raise x
    except Exception, e:
        print type(e)

# Both print "Exception"
throw(Exception)
throw(Exception())

def catches(t, e):
    try:
        raise e
    except t:
        return True
    except:
        return False

print catches(Exception, Exception)
print catches(Exception, Exception())
print catches(Exception(), Exception())
print catches(Exception, StopIteration())
print catches(Exception, RuntimeError())
print catches(Exception, KeyboardInterrupt())
print catches(None, Exception())
print catches(1, Exception())

_StopIteration = StopIteration
def f1():
    import __builtin__
    __builtin__.StopIteration = StopIteration = 0

    # This should still work:
    for i in xrange(5):
        print i

    __builtin__.StopIteration = _StopIteration
f1()

def f2(throw):
    print "f2"
    try:
        try:
            print "inner try"
            if throw:
                raise Exception()
        except:
            print "inner except"
        else:
            print "inner else"
            raise Exception()
    except:
        print "outer except"
    else:
        print "outer else"
f2(True)
f2(False)

def f3():
    # Finally blocks are actually somewhat complicated, because
    # you need to catch not just exceptions, but also continue/break/return's
    print "f3"

    for i in xrange(5):
        try:
            if i == 3:
                break
            continue
        finally:
            print "finally", i

    try:
        # Looks like this returns from the function, but it needs to go to the finally block
        return
    finally:
        print "in finally"
f3()

def f4():
    # Make sure that simply accessing a name can throw exceptions as expected

    print "f4"

    with expected_exception(NameError):
        print doesnt_exist

    global doesnt_exist2
    with expected_exception(NameError):
        print doesnt_exist2
f4()

def f5():
    # Make sure that we don't accidentally set 'x' here;
    # the exception prevents the assignment from happening
    print "f5"

    try:
        try:
            x = doesnt_exist
        except NameError:
            print "inner except"
            print x
    except NameError:
        print "outer except"

    # Similar test, except now the reference to 'y' *does* come from
    # the line that will end up throwing, but from a previous iteration
    # that didn't throw.
    def inner(i):
        if i == 1:
            raise AttributeError
    for i in xrange(5):
        try:
            y = inner(i)
        except AttributeError:
            print y
f5()

def f6():
    print "f6"

    a = 0

    with expected_exception(AttributeError):
        a.x = 0

    with expected_exception(TypeError):
        a[0] = 0

    # Tricky!
    # Note: a multiple-assignment statement like this is processed by setting the targets one by one, left-to-right.
    # So the assignment to "x" should succeed, but then the assignment to x.a will fail.
    # In the exception handler we should be able to see a value for x, but accessing y should fail.
    try:
        x = x.a = y = 1
        raise Exception("shouldn't get here")
    except AttributeError:
        print "caught, as expected"
        print "x = ", x
        try:
            print "y = ", y
            raise Exception("shouldn't get here")
        except UnboundLocalError:
            print "caught, as expected"
f6()

def f7():
    print "f7"

    # Make sure that desugaring produces exception handling as expected:

    class NonIterable(object):
        pass

    with expected_exception(TypeError):
        for i in NonIterable():
            pass

    class BadIterable(object):
        def __iter__(self):
            return self

        def next(self):
            raise NotImplementedError()

    with expected_exception(NotImplementedError):
        for i in BadIterable():
            print i

    class ExceptionRaiser(object):
        def __nonzero__(self):
            raise TestException()

        def __repr__(self):
            raise TestException()

    with expected_exception(TestException):
        while ExceptionRaiser():
            pass

    with expected_exception(TestException):
        print ExceptionRaiser()

    with expected_exception(TestException):
        assert ExceptionRaiser()

    with expected_exception(AssertionError):
        assert 0

    def throw():
        raise TestException()

    with expected_exception(TestException):
        def f(x=throw()):
            pass

    with expected_exception(TestException):
        class C(object):
            throw()

    with expected_exception(ImportError):
        import hopefully_this_package_doesnt_exist
        hopefully_this_package_doesnt_exist # to silence pyflakes

    with expected_exception(ImportError):
        from hopefully_this_package_doesnt_exist import a
        a # to silence pyflakes

    with expected_exception(TestException):
        print 1 if throw() else 0

    with expected_exception(TestException):
        print ExceptionRaiser() and 1

    with expected_exception(TestException):
        if throw():
            pass

    with expected_exception(TestException):
        if ExceptionRaiser():
            pass

    try:
        # Imports also need to be broken into separate parts:
        from sys import path, doesnt_exist
    except ImportError:
        print type(path)
        with expected_exception(NameError):
            print doesnt_exist
f7()

def f8():
    print "f8"

    def f(exc):
        print "evaluating except type;", exc.__name__
        return exc

    try:
        raise AttributeError()
    except f(TypeError):
        print "shouldn't be here 1"
    except f(AttributeError):
        print "should hit this"
    except f(NotImplementedError):
        print "shouldn't be here"
f8()

def f9():
    print "f9"

    # arithmetic

    with expected_exception(ZeroDivisionError):
        1/0
    with expected_exception(ZeroDivisionError):
        1.0/0
    with expected_exception(ZeroDivisionError):
        1/0.0
    with expected_exception(ZeroDivisionError):
        1 % 0
    with expected_exception(ZeroDivisionError):
        1 % 0.0
    with expected_exception(ZeroDivisionError):
        1.0 % 0

    with expected_exception(AttributeError):
        (1).a
f9()

def f10():
    print "f10"
    try:
        raise ZeroDivisionError()
    except:
        with expected_exception(ZeroDivisionError):
            raise
f10()
