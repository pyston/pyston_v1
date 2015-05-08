try:
    print intern(123)
except TypeError:
    print "caught expected TypeError"

print intern("abcd")

class StringSubclass(str):
    pass

# CPython does not allow interning on subclasses of str
try:
    print intern(StringSubclass())
except TypeError:
    print "caught expected TypeError from subclassing"
