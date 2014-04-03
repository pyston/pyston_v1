# Simple function call microbenchmark.
# Not really that interesting, but useful because it does a lot of work without any control flow

def f0():
    return 1
def f1():
    return f0() + f0()
def f2():
    return f1() + f1()
def f3():
    return f2() + f2()
def f4():
    return f3() + f3()
def f5():
    return f4() + f4()
def f6():
    return f5() + f5()
def f7():
    return f6() + f6()
def f8():
    return f7() + f7()
def f9():
    return f8() + f8()
def f10():
    return f9() + f9()
def f11():
    return f10() + f10()
def f12():
    return f11() + f11()
def f13():
    return f12() + f12()
def f14():
    return f13() + f13()
def f15():
    return f14() + f14()
def f16():
    return f15() + f15()
def f17():
    return f16() + f16()
def f18():
    return f17() + f17()
def f19():
    return f18() + f18()
def f20():
    return f19() + f19()
def f21():
    return f20() + f20()
def f22():
    return f21() + f21()
def f23():
    return f22() + f22()
def f24():
    return f23() + f23()
def f25():
    return f24() + f24()
def f26():
    return f25() + f25()
def f27():
    return f26() + f26()
def f28():
    return f27() + f27()
def f29():
    return f28() + f28()
def f30():
    return f29() + f29()
print f25()
