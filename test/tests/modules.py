import empty_module
import math
import types

x = repr(empty_module)

# cpython will simplify the path (remove . and ..), and use the pyc file in the repr string. Pyston still doesn't support that, so we are only checking the non-path part.

print x[0:29]
print x[-2:]

print repr(math)[:10]

m = types.ModuleType("foo")
print m.__doc__, type(m.__doc__)
m = types.ModuleType("foo", "bar")
print m.__doc__, type(m.__doc__)
m = types.ModuleType("foo", u"bar")
print m.__doc__, type(m.__doc__)
m.__init__("bar", "baz")
print m.__doc__, type(m.__doc__)
m.__dict__[u"\u20ac"] = "test"
