import sys
print list(enumerate(range(100)))
print list(enumerate(range(100), sys.maxint-50))

# cycle collection:
print enumerate(range(100)).next()

it = iter(range(5))
e = enumerate(it)
print e.next(), e.next()
print list(it)
