# Some corner cases around name scopes

def f1():
    exec ""
    from sys import version as sys_version
f1()

def f2():
    exec ""

    def f3(*args, **kw):
        exec ""
        print args, kw

    f3(*[1, 2], **dict(a=1, b=2))
f2()

__module__ = 1
__name__ = 2
__doc__ = 3
print __module__ # prints "1"
print __name__ # prints "2"
print __doc__ # prints "3"
class C(object):
    "hello world"

    # C's implicit setting of "__module__" will end up propagating to the global scope:
    global __module__
    global __name__
    global __doc__
print __module__ # prints "2"
print __name__ # prints "2"
print __doc__ # prints "hello world"

def f3():
    # All execs are name-forcing:
    exec "g='hi world'" in {}, locals()
    print g
f3()
