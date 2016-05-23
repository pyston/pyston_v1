# Make sure we can support weakrefs on extension objects.
# The _sre.SRE_Pattern type is one of the few builtin types that supports weakrefs natively.

import _sre

def f(n):
    if n:
        return f(n-1)
    p = _sre.compile('', 0, [1])
    print type(p)
    return weakref.ref(p)

import weakref
l = []
for i in xrange(5):
    l.append(f(20))

import gc
gc.collect()

assert any([r() is None for r in l])
