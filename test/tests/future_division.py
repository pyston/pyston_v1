# allow-warning

# The __future__ module has an old-style class, so we allow warnings for now

"docstring"

from __future__ import division

def test(a, b):
    print a, '/', b, '=', a / b
    t = a
    t /= b
    print a, '/', b, '=', t

    print a, '//', b, '=', a // b
    t = a
    t //= b
    print a, '//', b, '=', t

test(3, 2)
test(3, 2.0)
test(3.0, 2)
test(3.0, 2.0)
