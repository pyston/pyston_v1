# This should raise a python level error, not an assertion in the compiler

try:
    print int.doesnt_exist
except AttributeError, e:
    print e
