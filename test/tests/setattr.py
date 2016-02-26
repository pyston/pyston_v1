class MyDescr(object):
    def __set__(self, inst, val):
        print type(self), type(inst), val

class Test(object):
    def _setattr__(self, name, val):
        print name, val
        object.__setattr__(self, name, val)

    foo = MyDescr()

def test(t):
    print "testing..."
    t.hello = "world1"
    t.hello = "world2"
    t.foo = 2

test(Test())
Test.__setattr__ = object.__dict__['__setattr__']
print "set setattr to object setattr"
test(Test())
Test.__setattr__ = Test._setattr__
print "changed setattr to custom setattr"
test(Test())
del Test.__setattr__
test(Test())


class MyDescriptor(object):
    def __get__(self, inst, val):
        print type(self), type(inst), type(val)
        return self

    def __call__(self, *args):
        print args
class Test(object):
    __setattr__ = MyDescriptor()

t = Test()
t.a = 1

object.__setattr__(t, u"ustr", "42")
print t.ustr
