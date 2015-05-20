
def f():
    print "f"
    return 2

print f.__call__()

def g():
    print "g"
    return 3

print type(f).__call__(f)
print type(f).__call__(g)

print f.__name__, f.func_name
f.__name__ = "New name"
print f.__name__, f.func_name
f.func_name = "f"
print f.__name__, f.func_name

print bool(f)

def func_with_defaults(a, b=1):
    print a, b
func_with_defaults(0)

print type(func_with_defaults.func_code)
print func_with_defaults.func_defaults
print func_with_defaults.__defaults__

try:
    func_with_defaults.func_defaults = [2]
except TypeError as e:
    print e

# del func_with_defaults.__defaults__
# func_with_defaults.__defaults__ = (1, 2)
# func_with_defaults()

def func_without_defaults():
    pass
print repr(func_without_defaults.__defaults__)

print func_without_defaults.func_globals == globals()
import os
print os.renames.__globals__ == os.__dict__
print os.renames.__globals__ == globals()
d = {}
exec "def foo(): pass" in d
print d["foo"].func_globals == d
