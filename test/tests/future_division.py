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

class PhysicalQuantity(float):
    def __new__(cls, value):
        return float.__new__(cls, value)

    def __div__(self, x):
        print('__div__ get called')
        return PhysicalQuantity(float(self) / float(x))

    def __rdiv__(self, x):
        print('__rdiv__ get called')
        return PhysicalQuantity(float(x) / float(self))

a = PhysicalQuantity(2.0)

print(a / 3.0)
