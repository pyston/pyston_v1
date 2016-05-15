# Tests to see if we add any extra refs to function arguments.

import sys
print sys.getrefcount(object())

def f(o):
    print sys.getrefcount(o)

# This gives 3 for CPython and our interpreter, but 2 for the llvm tier:
# f(object())
