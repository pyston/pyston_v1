import operator

print operator.methodcaller
print operator.itemgetter
print operator.attrgetter

f = operator.methodcaller("__repr__")
print f(1)
print f("hello world")

f = operator.itemgetter(0)
print f("i")
print f((None,))

f = operator.attrgetter("count")
print f(["a", "a"])("a")
print f("ababa")("a")

for op in sorted(dir(operator)):
    if op.startswith("_"):
        continue
    print getattr(operator, op).__name__

a = range(4)
operator.setitem(a, 1, 3)
