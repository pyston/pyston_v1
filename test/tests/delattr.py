#delete attribute of built-in types 
def del_builtin_attr(o):
    try:
        del a.__init__
        print "error"
    except AttributeError, e:
    #print repr(e)
        print e

a=1    
del_builtin_attr(a)
a=1.0   
del_builtin_attr(a)
a=[1]
del_builtin_attr(a)
b={}   
del_builtin_attr(a)

"""
test del procedure
> invoke __delattr__ if exists
> del instance __dict__
> if attr is not in __dict__ of instance, check if attr exists in the its class.
     >if found, invoke __delete__ attribute if exists or throw AttributeError.
     >if not, throw AttributeError
"""

class CustDelClass(object):
    def __init__(self, val):
        self.val = val
    def __delattr__(self, attr):
        print "del attribute %s"%(attr)
a = CustDelClass(1)
del a.val
print a.val

class AttrClass(object):
#    
#classAttr = 1
   
    def __init__(self, val):
        self.val = val
    def speak(self):
        #print "val:%s, classAttr:%d"%(self.val, self.classAttr)
        print "val%s"%(self.val)
#check the val of b should not be affected
a = AttrClass(1)
b = AttrClass(2)
a.speak()
del a.val
try:
    a.speak()
    print "del a.val error"
except AttributeError, e:
    print e

try:
    b.speak()
    print "success"
except AttributeError, e:
    print "error"
#could assign a new value
a.val = 1
try:
    a.speak()
    print "success"
except AttributeError, e:
    print "error"

"""
try:
    del a.classAttr
    print "error"
except AttributeError, e:
    print repr(e)
"""

try:
    del a.speak
    print "del func error"
except AttributeError, e:
    print "del func:%s"%(e)

del AttrClass.speak
try:
    a.speak()
    print "error"
except AttributeError, e:
    print e


#test custom del attr
class CustDelClass(object):
    def __init__(self, val):
        self.val = val
    def __delattr__(self, attr):
        print "__delattr__ %s"%(attr)
      
    def speak(self):
        print "val:%s"%(self.val)

a=CustDelClass(1)
del a.attr

#custom set and get attr
class CustSetClass(object):
    def __setattr__(self, attr, val):
        print "__setattr__ %s %s"%(attr, val)
    #def __getattr__(self, attr):
    def __getattribute__(self, attr):
        print "__getattribute__:%s"%(attr)
        return attr

a=CustSetClass()
a.attr
a.val
#del won't invoke __getattr__ or __getattribute__ to check wether attr is an attribute of a
try:
    del a.attr
except AttributeError, e:
    print e


def f(o):
    print o.a

class C(object):
    pass

c = C()
c.a = 1

f(c)
del c.a
try:
    f(c)
    print "f(c) error"
except AttributeError, e:
    print e
