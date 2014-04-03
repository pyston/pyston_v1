def ident(x):
    return x
def f0():
    print "f0"
    return 0
def f1(a1):
    print "f1", a1
    return 0
def f2(a1, a2):
    print "f2", a1, a2
    return 0
def f3(a1, a2, a3):
    print "f3", a1, a2, a3
    return 0
def f4(a1, a2, a3, a4):
    print "f4", a1, a2, a3, a4
    return 0
def f5(a1, a2, a3, a4, a5):
    print "f5", a1, a2, a3, a4, a5
    return 0
def f6(a1, a2, a3, a4, a5, a6):
    print "f6", a1, a2, a3, a4, a5, a6
    return 0
def f7(a1, a2, a3, a4, a5, a6, a7):
    print "f7", a1, a2, a3, a4, a5, a6, a7
    return 0
def f8(a1, a2, a3, a4, a5, a6, a7, a8):
    print "f8", a1, a2, a3, a4, a5, a6, a7, a8
    return 0
def f9(a1, a2, a3, a4, a5, a6, a7, a8, a9):
    print "f9", a1, a2, a3, a4, a5, a6, a7, a8, a9
    return 0
def f10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10):
    print "f10", a1, a2, a3, a4, a5, a6, a7, a8, a9, a10
    return 0


for i in xrange(5):
    f0()
    f1(1)
    f2(1, 2)
    f3(1, 2, 3)
    f4(1, 2, 3, 4)
    f5(1, 2, 3, 4, 5)
    f6(1, 2, 3, 4, 5, 6)
    f7(1, 2, 3, 4, 5, 6, 7)
    f8(1, 2, 3, 4, 5, 6, 7, 8)
    f9(1, 2, 3, 4, 5, 6, 7, 8, 9)
    f10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    ident(f0)()
    ident(f1)(1)
    ident(f2)(1, 2)
    ident(f3)(1, 2, 3)
    ident(f4)(1, 2, 3, 4)
    ident(f5)(1, 2, 3, 4, 5)
    ident(f6)(1, 2, 3, 4, 5, 6)
    ident(f7)(1, 2, 3, 4, 5, 6, 7)
    ident(f8)(1, 2, 3, 4, 5, 6, 7, 8)
    ident(f9)(1, 2, 3, 4, 5, 6, 7, 8, 9)
    ident(f10)(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
