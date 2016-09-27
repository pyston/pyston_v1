# Test for various defaults arguments in builtin functions:

import os
import sys
sys.path.append(os.path.dirname(__file__) + "/../lib")
from test_helper import expected_exception

d = {}
print d.get(1)
print d.setdefault(2)
print d.pop(2)
print d.pop(2, None)
print d.pop(2, None)
with expected_exception(KeyError):
    print d.pop(2)

print min([1])
print min([1], None)

with expected_exception(AttributeError):
    print getattr(object(), "")
print getattr(object(), "", None)

print range(5)
with expected_exception(TypeError):
    print range(5, None)
print range(5, 10)

print list(xrange(5))
with expected_exception(TypeError):
    print list(xrange(5, None))
print list(xrange(5, 10))
