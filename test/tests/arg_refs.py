# skip-if: '-n' in EXTRA_JIT_ARGS or '-L' in EXTRA_JIT_ARGS
# Tests to see if we add any extra refs to function arguments.

import sys
print sys.getrefcount(object())

def f(o):
    print sys.getrefcount(o)

# This gives 3 for CPython and our interpreter, but 2 for the llvm tier:
f(object())

import sys
class C(object):
    def foo(self, *args):
        print sys.getrefcount(self)
c = C()
a = (c.foo)
print sys.getrefcount(c)
c.foo()
