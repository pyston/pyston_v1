# expected: fail
# - we don't stop tracebacks at the catching except handler.  this is hard do the way it gets added to
# (ie a bare "raise" statement will add more traceback entries to the traceback it raises)

import sys
import traceback

def f1():
    print "f1"
    def f2():
        try:
            1/0
        except:
            traceback.print_exc(file=sys.stdout)
            raise

    try:
        f2()
    except:
        traceback.print_exc(file=sys.stdout)
f1()

