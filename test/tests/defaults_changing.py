def func(a, b=1):
    print a, b
func(0)

print func.func_defaults, func.__defaults__

try:
    func.func_defaults = [2]
except TypeError as e:
    print e

print
print "Changing the default value"
func.func_defaults = (2,)
func(0)

print
print "Clearing the default"
func.func_defaults = ()
try:
    func(0)
except TypeError as e:
    print e

func.__defaults__ = (1, 2)
func()

del func.__defaults__
print "after del:", func.__defaults__
try:
    func(0)
except TypeError as e:
    print e

# You can specify more defaults than arguments:
func.func_defaults = (1, 2, 3, 4, 5)
print "after extra defaults:", func.__defaults__
func()

func.func_defaults = None
print "after setting to None:", func.__defaults__
try:
    func(0)
except TypeError as e:
    print e


# Test setting it to a subclass of tuple:
def f(a):
    print a
class MyTuple(tuple):
    def __getitem__(self, idx):
        print idx
        return 1

f.func_defaults = MyTuple((1, 2))
print type(f.__defaults__)
f()


class C(object):
    def __new__(cls, arg):
        print arg
        return object.__new__(cls)

    def foo(self, arg):
        print arg

print type(C.__new__), type(C.__dict__['__new__'])

C.__new__.__defaults__ = (1,)
print type(C())

def f(a=1, b=2, c=3):
    print a, b, c, type(a), type(b), type(c)
    f.func_defaults = (a + 3, b + 3, c + 3)
for i in xrange(1000):
    f()
