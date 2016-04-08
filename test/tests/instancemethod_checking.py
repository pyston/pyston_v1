# expected: fail

class C(object):
    def f(self):
        print "f", self
try:
    C.f()
except Exception as e:
    print e
try:
    C.f(1)
except Exception as e:
    print e


class M(type):
    pass

class M2(type):
    def __new__(*args):
        return type.__new__(*args)

    def other(*args):
        pass

print type(type.__new__)
print type(M.__new__)
print type(M2.__new__)

type.__new__(M, "my type", (object,), {})
M.__new__(M, "my type", (object,), {})
M2.__new__(M, "my type", (object,), {})

type.__new__(M2, "my type", (object,), {})
M.__new__(M2, "my type", (object,), {})
M2.__new__(M2, "my type", (object,), {})

try:
    M2.other(M2, "my type", (object,), {})
except Exception as e:
    print e
# for i in xrange(2):
    # print C.__new__(C)


# TODO: move this back to descriptors_nonfunc_types after this is working again:
import traceback
import sys
class InstanceMethodMaker(object):
    def getBoundInstanceMethod(*args):
        print '__get__ bound', map(type, args)
    def setBoundInstanceMethod(*args):
        print '__set__ bound', map(type, args)
    def getUnboundInstanceMethod(*args):
        print '__get__ unbound', map(type, args)
    def setUnboundInstanceMethod(*args):
        print '__set__ unbound', map(type, args)
imm = InstanceMethodMaker()

class A(object):
    class DescGetUnboundInstanceMethod(object):
        __get__ = InstanceMethodMaker.getUnboundInstanceMethod
    descGetUnboundInstanceMethod = DescGetUnboundInstanceMethod()
    class DescSetUnboundInstanceMethod(object):
        __set__ = InstanceMethodMaker.setUnboundInstanceMethod
    descSetUnboundInstanceMethod = DescSetUnboundInstanceMethod()
    class DescGetSetUnboundInstanceMethod(object):
        def __get__(*args):
            print 'DescGetSetUnboundInstanceMethod __get__ called', map(type, args)
        __set__ = imm.setUnboundInstanceMethod
    descGetSetUnboundInstanceMethod = DescGetSetUnboundInstanceMethod()

a = A()

print 'unbound instance method'
try:
    print a.descGetUnboundInstanceMethod
except:
    traceback.print_exc(file=sys.stdout)
try:
    a.descSetUnboundInstanceMethod = 5
except:
    traceback.print_exc(file=sys.stdout)
a.__dict__['descGetSetUnboundInstanceMethod'] = 3
print a.descGetSetUnboundInstanceMethod

