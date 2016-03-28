import sys
print list(enumerate(range(100)))
print list(enumerate(range(100), sys.maxint-50))

# cycle collection:
print enumerate(range(100)).next()
