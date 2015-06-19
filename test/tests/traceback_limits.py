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

