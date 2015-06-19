def f(x, y):
    print x + y
    print x - y
    print x * y
    print x / y
    print x // y
    print x % y
    print x ** y
    print x & y
    print x | y
    print x ^ y
    print x << y
    print x >> y
    print ~x

f(1, 2)
f(-1, 2)
f(0, 5)

print 1.0 % 0.3
print 2.5 % 2
print -1.0 % 0.3
print -2.5 % 2
print 1.0 % -0.3
print 2.5 % -2
print -1.0 % -0.3
print -2.5 % -2

print 1.0 ** 0.3
print 2.5 ** 2
print -1.0 ** 0.3
print -2.5 ** 2

try:
    f(2.0, 1.0)
except TypeError, e:
    print e

try:
    f(2.0, 1)
except TypeError, e:
    print e

try:
    f(2, -2)
except ValueError, e:
    print e
