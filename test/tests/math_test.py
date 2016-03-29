import math
print math.sqrt(2)
print math.sqrt(0)

print math.pi
print math.tan(math.pi)

print abs(1.0)
print abs(1)
print abs(-1)
print abs(-1.0)

print max(1, 2)
print min(1, 2)

print max(range(5))
print min(range(5))

print min(['aaa', 'bbb', 'c'])
print min(1, 2, 3)
print min(0.1, 1.4, 12.7)
print min('a', 'b', 'c')
print min('aaa', 'bbb', 'c')
print min('1', 2, 3, 'aa')
print min([1, 2, 3])
print min([0.1, 1.4, 12.7])
print min(['a', 'b', 'c'])
print min(['aaa', 'bbb', 'c'])
print min(['1', 2, 3, 'aa'])

try:
    min(1)
except TypeError as e:
    print e.message

try:
    min()
except TypeError as e:
    print e.message

try:
    min([])
except ValueError as e:
    print e.message

print max(1, 2, 3)
print max(0.1, 1.4, 12.7)
print max('a', 'b', 'c')
print max('aaa', 'bbb', 'c')
print max('1', 2, 3, 'aa')
print max([1, 2, 3])
print max([0.1, 1.4, 12.7])
print max(['a', 'b', 'c'])
print max(['aaa', 'bbb', 'c'])
print max(['1', 2, 3, 'aa'])

try:
    max(1)
except TypeError as e:
    print e.message

try:
    max()
except TypeError as e:
    print e.message

try:
    max([])
except ValueError as e:
    print e.message

# test with key function
lst = [2, 1, 3, 4]
print(min(lst, key=lambda x: x))
print(min(lst, key=lambda x: -x))
print(min(1, 2, 3, 4, key=lambda x: -x))
print(min(4, 3, 2, 1, key=lambda x: -x))

print(max(lst, key=lambda x: x))
print(max(lst, key=lambda x: -x))
print(max(1, 2, 3, 4, key=lambda x: -x))
print(max(4, 3, 2, 1, key=lambda x: -x))

print min([[1, 2], [3, 4], [9, 0]], key=lambda x: x[1])
print min(1.2, 6.3, 6.9, key=int)
print min("moon", "sun", "earth", key=len)

print max([[1, 2], [3, 4], [9, 0]], key=lambda x: x[1])
print max(1.2, 6.3, 6.9, key=int)
print max("moon", "sun", "earth", key=len)

try:
    min(1, a=1)
except TypeError as e:
    print e.message

try:
    max(1, a=1)
except TypeError as e:
    print e.message

try:
    min(1, 2, key=lambda x, y: x+y)
except TypeError as e:
    print e.message

try:
    max(1, 2, key=lambda x, y: x+y)
except TypeError as e:
    print e.message

try:
    min([1], key=lambda x: x, extra_arg=1)
except TypeError:
    print e.message

try:
    max([1], key=lambda x: x, extra_arg=1)
except TypeError:
    print e.message


class C(object):
    def __init__(self, n):
        self.n = n

    def __lt__(self, rhs):
        print "lt", self.n, rhs.n
        return self.n < rhs.n

    def __gt__(self, rhs):
        print "gt", self.n, rhs.n
        return self.n > rhs.n


def key(x):
    print "key", x.n
    return C(-x.n)

print min([C(1), C(3), C(2)], key=key).n
print max([C(1), C(3), C(2)], key=key).n

for x in [float("inf"), math.pi]:
    print x, math.isinf(x), math.fabs(x), math.ceil(x), math.log(x), math.log10(x)

print math.sqrt.__name__
