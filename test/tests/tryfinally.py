# try-finally support

import sys

# Basic finally support:
print "basic_finally"
def basic_finally(n):
    try:
        1/n
        print "1"
    except:
        print "2"
    else:
        print "3"
    finally:
        print "4"
    print "5"
print basic_finally(1)
print basic_finally(0)
print

# If we return from inside the try part of a try-finally, we have to save the return value,
# execute the finally block, then do the actual return.
print "finally_after_return"
def finally_after_return():
    try:
        print 1
        return 2
    finally:
        print 3
    print 4
print finally_after_return()
print

# Return from a finally will disable any exception propagation:
print "return_from_finally"
def return_from_finally(to_throw=None):
    try:
        if to_throw:
            raise to_throw
        return 1
    except:
        print "except"
        return 2
    else:
        print "else"
        return 3
    finally:
        print "finally"
        return 4
print return_from_finally()
print return_from_finally(Exception)
print

# A break in a finally will disable any exception propagation
# Note: "break" is allowed in a finally, but "continue" is not (see finally_continue.py)
print "break_from_finally"
def break_from_finally(to_throw=None):
    for i in xrange(5):
        print i
        try:
            if to_throw:
                raise to_throw
            return 1
        except:
            return 2
        else:
            return 3
        finally:
            break
    return 4
print break_from_finally(None)
print break_from_finally(Exception)
print

# I guess you're allowed to yield from a finally block.
# Once execution is returned, exception propagation will continue
print "yield_from_finally"
def yield_from_finally(to_throw=None):
    for i in xrange(5):
        try:
            if to_throw:
                raise to_throw
        finally:
            yield i
for i in yield_from_finally():
    print i
try:
    for i in yield_from_finally(Exception("ex")):
        print i
        # Throw a different exception just for fun:
        try:
            raise Exception()
        except:
            pass
except Exception, e:
    print e
print

# Similarly for continues
print "finally_after_continue"
def finally_after_continue():
    for i in xrange(5):
        try:
            continue
        finally:
            print 3
print finally_after_continue()
print

# And breaks
print "finally_after_break"
def finally_after_break():
    for i in xrange(5):
        try:
            break
        finally:
            print 3
print finally_after_break()
print

# Exceptions thrown in the else or except blocks of a try-finally still run the finally
print "exception_in_elseexcept"
def exception_in_elseexcept(throw0=None, throw1=None, throw2=None):
    try:
        if throw0:
            raise throw0
    except:
        print "except"
        if throw1:
            raise throw1
    else:
        print "else"
        if throw2:
            raise throw2
    finally:
        print "in finally"
for t0 in [None, Exception("exc 0")]:
    for t1 in [None, Exception("exc 1")]:
        for t2 in [None, Exception("exc 2")]:
            print "throwing:", t0, t1, t2
            try:
                exception_in_elseexcept(t0, t1, t2)
                print "no exception"
            except Exception, e:
                print "threw:", e
print

# An exception thrown and caught inside a finally doesn't hide the current exception propagation
print "exception_in_finally"
def exception_in_finally():
    try:
        1/0
    finally:
        print sys.exc_info()[0]
        try:
            raise KeyError()
        except KeyError:
            pass
        print sys.exc_info()[0]
try:
    print exception_in_finally()
    print "no exception"
except ZeroDivisionError, e:
    print e
print

# sys.exc_clear() doesn't stop finally-exception propagation
print "sysclear_in_finally"
def sysclear_in_finally():
    try:
        1/0
    finally:
        sys.exc_clear()
try:
    print sysclear_in_finally()
    print "no exception"
except ZeroDivisionError, e:
    print e
print

# An uncaught exception from a finally will override the previous exception:
print "raise_from_finally"
def raise_from_finally():
    try:
        raise Exception("exception 1")
    finally:
        raise Exception("exception 2")
try:
    raise_from_finally()
except Exception, e:
    print e
print

# Make sure we can handle various nestings of try-finally
print "nested_finally"
def nested_finally():
    try:
        try:
            for i in xrange(5):
                pass
        finally:
            print "finally1"
    finally:
        print "finally2"
        try:
            for j in xrange(5):
                pass
        finally:
            print "finally3"
nested_finally()
print

# finally blocks hide their exceptions even from bare "raise" statements:
print "bare_raise_in_finally"
def bare_raise_in_finally():
    try:
        raise Exception("first exception")
    except:
        pass

    try:
        1/0
    finally:
        raise # raises the "first exception" exception above
try:
    bare_raise_in_finally()
except Exception, e:
    print e
print

# Some older tests.  Keep them around, because why not

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

            print sys.exc_info()[0]
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
    except TypeError, e:
        print "Got TypeError as expected, since exc_info was None"
        print e
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

