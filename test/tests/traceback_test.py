# A test of both the tracebacks we generate and the traceback module
#
# (We keep fixing tracebacks in one case to break them in another, so it's time for a test.)
#
# All of these tests involve except handlers at the top scope, since we currently generate extra-long
# tracebacks when an exception gets caught inside a function.
# - In CPython the traceback goes from the point of the exception to the point of the handler
# - In Pyston the traceback always goes to the top-most frame
#
# See traceback_limits.py

import sys
import traceback

try:
    1/0
except:
    traceback.print_exc(file=sys.stdout)

def f():
    traceback.print_exc(file=sys.stdout)
try:
    1/0
except:
    f()
traceback.print_exc(file=sys.stdout)

try:
    1/0
except:
    a, b, c = sys.exc_info()

    try:
        [][1]
    except:
        pass

    traceback.print_exc(file=sys.stdout)

    try:
        raise a, b, c
    except:
        traceback.print_exc(file=sys.stdout)
traceback.print_exc(file=sys.stdout)

def f():
    1/0

try:
    f()
except:
    traceback.print_exc(file=sys.stdout)

def f():
    def g():
        1/0
    g()
try:
    f()
except:
    a, b, t = sys.exc_info()
    # For print_tb, the 'limit' parameter starts from the bottommost call frame:
    traceback.print_tb(t, limit=1)

try:
    1/0
except AttributeError:
    pass
except:
    traceback.print_exc(file=sys.stdout)

try:
    try:
        1/0
    except AttributeError:
        pass
except:
    traceback.print_exc(file=sys.stdout)

try:
    try:
        1/0
    except:
        raise
except:
    traceback.print_exc(file=sys.stdout)

try:
    raise AttributeError()
except:
    traceback.print_exc(file=sys.stdout)

def f():
    1/0
try:
    f()
except:
    traceback.print_exc(file=sys.stdout)

    a, b, t = sys.exc_info()
    try:
        raise a, b, None
    except:
        traceback.print_exc(file=sys.stdout)

def f(n):
    if n:
        return f(n - 1)
    traceback.print_stack(file=sys.stdout)
    print
    return traceback.format_stack()
print
print ''.join(f(5))


# Output some extra stuff at the end so that it doesn't look like the script crashed with an exception:
print
print "done!"
