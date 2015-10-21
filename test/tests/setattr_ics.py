# skip-if: '-n' not in EXTRA_JIT_ARGS and '-O' not in EXTRA_JIT_ARGS
# statcheck: noninit_count('slowpath_setattr') < 50

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
    t.hello = "world"
    t.hello = "world"
    t.foo = 2

for i in xrange(100):
    test(Test())
Test.__setattr__ = object.__dict__['__setattr__']
print "set setattr to object setattr"
for i in xrange(100):
    test(Test())
Test.__setattr__ = Test._setattr__
print "changed setattr to custom setattr"
test(Test())
del Test.__setattr__
for i in xrange(100):
    test(Test())

class Old():
    pass
old = Old()
for i in xrange(1000):
    old.a = i
    assert old.a == i

