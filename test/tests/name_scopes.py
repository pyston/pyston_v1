# Some corner cases around name scopes

def f1():
    exec ""
    from os import path as os_path
    print os_path.join("a", "b")
f1()

def f2():
    exec ""

    def f3(*args, **kw):
        exec ""
        print args, kw

    f3(*[1, 2], **dict(a=1, b=2))
f2()
