import operator

print reduce(operator.add, range(50))
print reduce(operator.add, range(40), 0)
print reduce(operator.add, "hello world")

print reduce(operator.add, "", 0)

try:
    print reduce(operator.add, "hello world", 0)
except TypeError, e:
    print e


def f(a, b):
    print "f", a, b
    return b
print reduce(f, "abc", 0)
print reduce(f, "abc")

try:
    print reduce(f, [])
except TypeError, e:
    print e
