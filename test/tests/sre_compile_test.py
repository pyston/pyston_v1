# skip-if: True
# This test works but 1) is very slow [the importing is, not the regex itself], and 2) throws warnings

import sre_compile
r = sre_compile.compile("a(b+)c", 0)
print r.match("")
print r.match("ac")
print r.match("abc").groups()
print r.match("abbc").groups()
