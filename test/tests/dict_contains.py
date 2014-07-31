# expected: fail

# using {} style works
print 1 in {}
print 1 in {1:1}
print 'a' in {}
print 'a' in {'a': 1}

# using dict fails
print 'a' in dict()
print 'a' in dict(a=1)
