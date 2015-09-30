# expected: fail

# CPython's str.__add__ directly throws a TypeError.
# Ours (and PyPy's) returns NotImplemented.

try:
    print "".__add__(1)
except TypeError as e:
    print e

try:
    print [].__add__(1)
except TypeError as e:
    print e
