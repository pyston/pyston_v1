import sys

def p():
    print hasattr(sys.modules['__main__'], 'a')

p()
a = 1
p()
del a
p()
try:
    del a
except NameError, e:
    print e


a = 1
def mk_cls(b1, b2):
    class C(object):
        if b1:
            a = 2
        if b2:
            # with b1==False and b2==True, this reference to 'a' will go to the global scope.
            # The 'del a' will still refer to the local scope and raise a NameError.
            print a
            del a
    print hasattr(C, 'a')

mk_cls(False, False)
mk_cls(True, False)
mk_cls(True, True)
try:
    mk_cls(False, True)
    assert 0
except NameError, e:
    print e


def f1(b1, b2):
    if b1:
        a = 2
    if b2:
        del a

f1(False, False)
f1(True, False)
f1(True, True)
try:
    f1(False, True)
    assert 0
except NameError, e:
    print e

def f2():
    del a
try:
    f2()
    assert 0
except NameError, e:
    print e

def f3():
    a = 1
    del a
    if 0:
        pass
    print locals()
f3()
