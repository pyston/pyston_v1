# expected: fail
# - crashes rather than throws an error

try:
    from . import doesnt_exist
except ImportError, e:
    print e
