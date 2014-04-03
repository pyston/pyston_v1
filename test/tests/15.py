# Regression test:
# use the same string constant from two different functions.  MCJIT will allow you to use a
# global variable from another module, but it will crash at deallocation.

def f1():
    print "hello world"
    return 0
def f2():
    print "hello world"
    return 0
f1()
f2()
