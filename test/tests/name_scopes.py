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
