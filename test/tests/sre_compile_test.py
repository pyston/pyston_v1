# skip-if: True
# This test works but 1) is very slow [the importing is, not the regex itself], and 2) throws warnings
# This test also seems to leak a lot of memory.

import sre_compile
r = sre_compile.compile("a(b+)c", 0)
print r.match("")
print r.match("ac")
print r.match("abc").groups()
for i in xrange(100000):
    r.match("abbc").groups()
    if i % 1000 == 0:
        print i
