import sre_compile
import sre_constants

r = sre_compile.compile("a(b+)c", 0)
print r.match("")
print r.match("ac")
print r.match("abc").groups()
for i in xrange(100000):
    r.match("abbc").groups()
    if i % 10000 == 0:
        print i

def identity(o):
    return o

charset = [(sre_constants.RANGE, (128, 65535))]

# Note: _optimize_charset changed behavior + interface some time
# between 2.7.7 (our stdlib) and 2.7.9 (current cpython latest),
# so this test has a .expected file to avoid running cpython.
print sre_compile._optimize_charset(charset, identity)
