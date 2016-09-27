# Make sure that all of these cases get recursion-checked appropriately

import os
import sys
sys.path.append(os.path.dirname(__file__) + "/../lib")
from test_helper import expected_exception

l = []
l2 = []
l.append(l)
l2.append(l2)

with expected_exception(RuntimeError):
    l == l2

def recurse():
    recurse()

with expected_exception(RuntimeError):
    recurse()

def f(n):
    if n > 0:
        f(n-1)

# If the 'exec' itself is the frame that triggers the recursion depth error,
# it will fail in an obscure way: "KeyError: 'unknown symbol table entry'"
# (This is because it fails during the compilation of the exec'd string.)
# So add some other calls just to try to avoid that
s = """
f(10)
exec s
"""
with expected_exception(RuntimeError):
    exec s

s = 'f(10), eval(s)'
with expected_exception(RuntimeError):
    eval(s)
