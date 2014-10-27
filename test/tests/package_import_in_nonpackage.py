# expected: fail
# - Relative imports not supported

try:
    from . import doesnt_exist
except ImportError, e:
    print e
