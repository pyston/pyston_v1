# closure tests

# simple closure:
def make_adder(x):
    def g(y):
        return x + y
    return g

a = make_adder(1)
print a(5)
print map(a, range(5))

def make_adder2(x2):
    # f takes a closure since it needs to get a reference to x2 to pass to g
    def f():
        def g(y2):
            return x2 + y2
        return g

    r = f()
    print r(1)
    x2 += 1
    print r(1)
    return r

a = make_adder2(2)
print a(5)
print map(a, range(5))

def make_addr3(x3):
    # this function doesn't take a closure:
    def f1():
        return 2
    f1()

    def g(y3):
        return x3 + y3

    # this function does a different closure
    def f2():
        print f1()
    f2()
    return g
print make_addr3(10)(2)

def f4(args):
     def inner():
       for a in args:
         print a
     return inner
print f4([1, 2, 3])()

def f5():
    x = 12039
    def i1():
        def i2():
            print x
        i2()
    i1()
f5()

def f6():
    x = 131
    # Regression test:
    # default args shouldn't mess with closure analysis
    def inner(a=1):
        print x
    inner()

    print (lambda a=1: x)()
f6()
