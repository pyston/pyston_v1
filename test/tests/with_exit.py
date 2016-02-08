# Make sure __exit__ gets called in various exit scenarios:

class NewC(object):
    def __enter__(self):
        print "__enter__"
    def __exit__(self, type, val, tb):
        print "__exit__"


class OldC:
    def __enter__(self):
        print "__enter__"
    def __exit__(self, type, val, tb):
        print "__exit__"

def f(C):
    with C():
        pass
    with C() as n:
        pass

    n = 2
    while n:
        print n
        n = n - 1
        with C() as o:
            continue

    n = 2
    while n:
        print n
        n = n - 1
        with C() as o:
            with C() as o2:
                continue
            continue

    n = 2
    while n:
        print n
        n = n - 1
        with C() as o:
            break

    n = 2
    while n:
        print n
        n = n - 1
        with C() as o:
            return

f(NewC)
f(OldC)

def f2(b, C):
    n = 2
    while n:
        print n
        n = n - 1
        with C() as o:
            with C() as o2:
                if b:
                    return "b true"
                else:
                    return "b false"

print f2(False, NewC), f2(False, OldC)
print f2(True, NewC), f2(True, OldC)

try:
    with None:
        print "inside"
except AttributeError as e:
    assert "__exit__" in str(e)
