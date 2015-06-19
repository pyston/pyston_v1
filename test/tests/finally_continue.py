# expected: fail
# - don't do this check yet

# Syntax error to have a "continue" in a finally
#
# In CPython the error gets triggered if you enter the file at all,
# even if you don't execute the function with the continue-in-finally.

def f():
    for i in xrange(5):
        try:
            pass
        finally:
            continue
f()
