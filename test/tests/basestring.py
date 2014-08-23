print isinstance("", basestring)
print isinstance(3, basestring)

print basestring.__doc__

# should raise an exception
t = basestring.__new__(basestring)
