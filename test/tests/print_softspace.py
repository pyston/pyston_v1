class C(object):
    def write(self, s):
        print "class write", repr(s)
c = C()

# CPython softspace special-case:
# if you print a string that ends with a newline, don't emit a space on the next print.
# This only applies if you're printing a string object, not to anything else even if they
# will get printed as a string with a newline at the end.
class R(object):
    def __str__(self):
        print "str"
        return "\n"
print >>c, R(),
print c.softspace
print >>c, "\n",
print c.softspace
