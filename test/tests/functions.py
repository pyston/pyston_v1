
def f():
    """very nice function"""
    print "f"
    return 2

print f.__call__()
print f.__doc__
print f.func_doc

def g():
    print "g"
    return 3

print g.__doc__

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

func_without_defaults.func_code = func_with_defaults.func_code
print func_without_defaults.func_name, func_without_defaults.func_code.co_name
try:
    func_without_defaults(2)
except TypeError, e:
    print e
func_without_defaults(2, 3)

def foo():
    return 0
def bar():
    return 1
s = 0
for i in xrange(1000):
    s += foo()
    if not i % 100:
        foo.func_code, bar.func_code = bar.func_code, foo.func_code
print s


# Copied from https://github.com/networkx/networkx/blob/master/networkx/algorithms/isomorphism/matchhelpers.py
import types
def copyfunc(f, name=None):
    """Returns a deepcopy of a function."""
    try:
        # Python <3
        return types.FunctionType(f.func_code, f.func_globals,
                                  name or f.__name__, f.func_defaults,
                                  f.func_closure)
    except AttributeError:
        # Python >=3
        return types.FunctionType(f.__code__, f.__globals__,
                                  name or f.__name__, f.__defaults__,
                                  f.__closure__)

def g(x, z=[]):
    z.append(x)
    print z
print g.func_closure
g2 = copyfunc(g)
assert g.func_defaults == g2.func_defaults, (g.func_defaults, g2.func_defaults)
g(1)
g2(2)


# Regression test: make sure that __globals__/func_globals gets set
# properly in exec cases
d = {}
exec """
def f():
    pass
""" in d
assert type(d['f'].__globals__) == dict

exec """
def f():
    pass
""" in globals()
assert type(globals()['f'].__globals__) == type(globals())



def f(self):
    pass

class C(object):
    pass

c = C()

import re
def s(o):
    return re.sub('0x[0-9a-f]+', '', str(o))

print s(f.__get__(c))
print s(f.__get__(c, C))
print s(f.__get__(c, c))

print f.__get__(c)()
print f.__get__(c, C)()
print f.__get__(c, None)()
print f.__get__(c, c)()
