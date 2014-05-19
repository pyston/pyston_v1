# expected: fail
# - exceptions

# Different ways of nesting exceptions

import sys

def f1():
    print
    print "f1"
    # First make sure that xrange iterator really does raise StopIteration:
    try:
        iter(xrange(0)).next()
        assert 0
    except StopIteration:
        print sys.exc_info()[0].__name__
f1()

def f2():
    print
    print "f2"
    try:
        raise Exception
    except Exception:
        print sys.exc_info()[0].__name__ # "Exception"

        for i in xrange(5):
            pass

        print sys.exc_info()[0].__name__ # "Exception", despite the implicit StopIteration that got raised

        def f():
            try:
                raise StopIteration()
            except:
                pass

        f()

        print sys.exc_info()[0].__name__ # still "Exception"

        def f2():
            raise StopIteration()

        def f3():
            try:
                f2()
            except StopIteration:
                pass

        try:
            f3()
        except:
            pass

        print sys.exc_info()[0].__name__ # still "Exception", since the exception didn't get back up to this frame

        try:
            f2()
        except:
            pass

        print sys.exc_info()[0].__name__ # "StopIteration"
f2()

def f2_2():
    try:
        raise Exception
    except Exception:
        # These two look similar, but they have different
        # exception-setting behavior:
        for n in xrange(5):
            print n, sys.exc_info()[0].__name__

        it = iter(xrange(5))
        while True:
            try:
                n = it.next()
            except StopIteration:
                break
            print n, sys.exc_info()[0].__name__

        print "done", n, sys.exc_info()[0].__name__
    print "after", n, sys.exc_info()[0].__name__
f2_2()

def f3():
    print
    print "f3"

    def f():
        print "getting the exc handler type"
        raise AssertionError()
    try:
        print "in the first try"
        # f() won't get evaluated until the exception is actually thrown:
        try:
            print "in the second try"
            raise Exception()
        except f():
            print "In the inner exception block??"
        finally:
            # This will get called even though there was an exception in
            # evaluating the exception-handler type:
            print "inner finally"
    except Exception:
        # This will print "AssertionError", from the f() call, *not* the Exception
        # that was thrown in the inner try block.
        print "In the outer exception block:", sys.exc_info()[0].__name__
    finally:
        print "outer finally"
f3()

def f4():
    print
    print "f4"

    # This test answers the question of what the exact behavior of the "raise" (no argument) statement is,
    # especially after a sub-exception has changed sys.exc_info

    try:
        try:
            raise AttributeError()
        except AttributeError:
            try:
                raise NotImplementedError()
            except:
                pass

            # Even though lexically it looks like we're handling an AttributeError,
            # at this point the "most recent exception" is a NotImplementedError,
            # so when we "raise" we should throw that.
            raise
    except AttributeError:
        print "caught attribute error (makes sense, but wrong)"
    except NotImplementedError:
        print "caught not implemented error (weird, but right)"
f4()

def f5():
    print
    print "f5"

    # Based on what I learned from f4, I guess you can put a "raise" outside a try-catch block:

    def inner():
        try:
            raise AttributeError()
        except:
            pass

        print "reraising"
        raise

    try:
        inner()
        assert 0, "shouldn't get here"
    except AttributeError:
        print sys.exc_info()[0].__name__
f5()

def f6():
    print
    print "f6"

    # A finally block must somehow track how it was entered, because it's not based
    # on the value of sys.exc_info at the end of the finally block:

    def inner(nested_throw, reraise):
        try:
            pass
        finally:
            if nested_throw:
                try:
                    raise AttributeError()
                except:
                    pass

            print sys.exc_info()
            if reraise:
                raise

    inner(False, False) # no exception raised
    inner(True, False) # no exception raised

    try:
        inner(True, True)
        # Shouldn't get here
        raise Exception()
    except AttributeError:
        print "the thrown AttributeError raised as expected"
    # Have to call this, because the inner throw can reraise the out-of-except
    # exception from this scope!
    sys.exc_clear()

    try:
        inner(False, True)
        # Shouldn't get here
        raise Exception()
    except TypeError:
        print "Got TypeError as expected, since exc_info was None"
f6()

def f7():
    print
    print "f7"

    # Similar test to f6, but this time with an exception propagating
    # up through a finally block.
    # An exception thrown inside that finally shouldn't change the exception
    # that will end up getting propagated

    def inner():
        try:
            raise AttributeError()
        finally:
            try:
                raise NotImplementedError()
            except:
                pass
            print sys.exc_info()[0].__name__
    try:
        inner()
    except:
        print sys.exc_info()[0].__name__
f7()

def f8():
    print
    print "f8"

    try:
        raise AttributeError()
    except:
        pass

    def reraise():
        raise

    try:
        reraise()
        raise Exception()
    except AttributeError:
        print "reraised correctly"
f8()

def f9():
    print
    print "f9"

    # Exceptions thrown inside a catch block should still go through the finally,
    # but not other catch blocks.

    try:
        try:
            raise Exception()
        except Exception:
            print "here"
            raise AttributeError()
        except AttributeError:
            print "shouldn't get here"
        finally:
            print "in finally"
    except AttributeError:
        pass
f9()

def f10():
    print "f10"

    x = 1
    try:
        try:
            y = 2
            raise AttributeError()
            x = 3
        except NotImplementedError:
            print "shouldn't be here"
    except AttributeError:
        print x, y
        print "here"
f10()
