# See what happens when we make __get__ and __set__ things other than functions...
# TODO add some with __del__

import sys
import traceback

class CallableGet(object):
    def __call__(*args):
        print "callable get", map(type, args)
class CallableSet(object):
    def __get__(*args):
        print "__get__", map(type, args)
        return args[0]
    def __call__(*args):
        print "callable set", map(type, args)

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

def closureGet():
    a = 5
    def f(*args):
        print 'closure __get__', map(type, args), a
    return f
def closureSet():
    a = 5
    def f(*args):
        print 'closure __set__', map(type, args), a
    return f

class A(object):
    # If __get__ or __set__ is an int
    class DescGetInt(object):
        __get__ = 1
    descGetInt = DescGetInt()
    class DescSetInt(object):
        __set__  = 1
    descSetInt = DescSetInt()
    class DescGetSetInt(object):
        def __get__(*args):
            print 'DescGetSetInt __get__ called', map(type, args)
        __set__ = 1
    descGetSetInt = DescGetSetInt()

    class DescGetCall(object):
        __get__ = CallableGet()
    descGetCall = DescGetCall()
    class DescSetCall(object):
        __set__ = CallableSet()
    descSetCall = DescSetCall()
    class DescGetSetCall(object):
        def __get__(*args):
            print 'DescGetSetCall __get__ called', map(type, args)
        __set__ = CallableSet()
    descGetSetCall = DescGetSetCall()

    class DescGetBoundInstanceMethod(object):
        __get__ = imm.getBoundInstanceMethod
    descGetBoundInstanceMethod = DescGetBoundInstanceMethod()
    class DescSetBoundInstanceMethod(object):
        __set__ = imm.setBoundInstanceMethod
    descSetBoundInstanceMethod = DescSetBoundInstanceMethod()
    class DescGetSetBoundInstanceMethod(object):
        def __get__(*args):
            print 'DescGetSetBoundInstanceMethod __get__ called', map(type, args)
        __set__ = imm.setBoundInstanceMethod
    descGetSetBoundInstanceMethod = DescGetSetBoundInstanceMethod()

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

    class DescGetClosure(object):
        __get__ = closureGet()
    descGetClosure = DescGetClosure()
    class DescSetClosure(object):
        __set__ = closureSet()
    descSetClosure = DescSetClosure()
    class DescGetSetClosure(object):
        def __get__(*args):
            print 'DescGetSetClosure __get__ called', map(type, args)
        __set__ = closureSet()
    descGetSetClosure = DescGetSetClosure()

    class DescGetGenerator(object):
        def __get__(*args):
            print 'DescGetGenerator __get__ called', map(type, args)
            yield 15
            print '__get__ post yield'
    descGetGenerator = DescGetGenerator()
    class DescSetGenerator(object):
        def __set__(*args):
            print 'DescSetGenerator __set__ called', map(type, args)
            yield 15
            print '__set__ post yield'
    descSetGenerator = DescSetGenerator()
    class DescGetSetGenerator(object):
        def __get__(a, b, c):
            print 'DescGetSetGenerator __get__ called'
            print a
            print b
            print c
        def __set__(self, obj, value):
            print 'DescGetSetGenerator __set__ called'
            print self
            print obj
            print value
            yield 15
            print 'DescGetSetGenerator __set__ post yield'
    descGetSetGenerator = DescGetSetGenerator()

    descSetClosure = DescSetClosure()

a = A()

print 'int'
try:
    print a.descGetInt
except:
    traceback.print_exc(file=sys.stdout)

try:
    a.descSetInt = 5
except:
    traceback.print_exc(file=sys.stdout)

a.__dict__['descGetSetInt'] = 3
print a.descGetSetInt

print 'object with __call__'
print a.descGetCall
a.descSetCall = 5
a.__dict__['descGetSetCall'] = 3
print a.descGetSetCall

print 'bound instance method'
print a.descGetBoundInstanceMethod
a.descSetBoundInstanceMethod = 5
a.__dict__['descGetSetBoundInstanceMethod'] = 3
print a.descGetSetBoundInstanceMethod

# TODO: uncomment this after instancemethod_checking is working
'''
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
'''

print 'closure'
print a.descGetClosure
a.descSetClosure = 5
a.__dict__['descGetSetClosure'] = 3
print a.descGetClosure

print 'generator'
print type(a.descGetGenerator)
a.descSetGenerator = 5
a.__dict__['descGetSetGenerator'] = 3
print type(a.descGetGenerator)

