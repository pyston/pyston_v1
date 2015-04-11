# used to trip an assert about vref counts in irgenerator.cpp: IrGeneratorImpl::doReturn()
def f1():
    def bar(x):
        print 'bar(%s)' % x
        if x:
            return bar(0)
        return x
    return bar

g = f1()
print g(2)

def makeA():
    class A(object):
        def __init__(self, *args):
            self.args = args

        @classmethod
        def _make(cls, *args):
            tmp = A
            return cls(*args)
    return A

def f2():
    A = makeA()
    return A(1,2,3)
a = f2()
print a.args
