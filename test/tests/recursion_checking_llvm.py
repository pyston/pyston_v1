# expected: fail
# - Recursion that happens entirely within the llvm tier does not get depth-checked

import os
import sys
sys.path.append(os.path.dirname(__file__) + "/../lib")
from test_helper import expected_exception

def f(n):
    if n > 0:
        f(n-1)

# force llvm-tier:
for i in xrange(3000):
    f(5)

with expected_exception(RuntimeError):
    f(100000000)
