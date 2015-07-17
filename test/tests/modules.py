import empty_module
import math

x = repr(empty_module)

# cpython will simplify the path (remove . and ..), and use the pyc file in the repr string. Pyston still doesn't support that, so we are only checking the non-path part.

print x[0:29]
print x[-2:]

print repr(math)[:10]
