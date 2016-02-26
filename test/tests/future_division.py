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
test(3.0, -2.0)
test(-3.0, -2.0)
test(1.0 + 1.0j, 2)
test(1.0 + 1.0j, 2.0)
test(1.0 + 1.0j, 2.0j)
