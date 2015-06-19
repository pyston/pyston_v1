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

print sre_compile._optimize_charset(charset, identity)
