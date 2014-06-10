import empty_module
import math

x = repr(empty_module)

# cpython will simplify the path (remove . and ..), and use the pyc file in the repr string. Pyston still doesn't support that, so we are only checking the non-path part.

print x[0:29]
print x[-2:]

# cpython 2.7.5 writes "from '/usr/lib64/python2.7/lib-dynload/math.so'"
# pyston writes "(built-in)"
print repr(math)[0:15] + "(built-in)>"
