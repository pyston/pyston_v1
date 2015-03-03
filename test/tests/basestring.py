print isinstance("", basestring)
print isinstance(3, basestring)

print basestring.__doc__

# should raise an exception
try:
    t = basestring.__new__(basestring)
except TypeError, e:
    print e
