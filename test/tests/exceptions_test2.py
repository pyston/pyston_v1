# this test caused crashes because of a missing deinitFrame call in the LLVM tier when compiling funcs in CAPI mode
def next(x):
    1/x

def foo(x):
    try:
        next(x)
    except Exception as e:
        print "exc", e

foo(1)
foo(0)
