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

s1 = "Test" 
s2 = " String"
print s1+s2 is s1+s2
print intern(s1+s2) is intern(s1+s2)
