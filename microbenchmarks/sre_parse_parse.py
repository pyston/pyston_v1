# sre_parse._parse() is a pretty hard function for us to jit
#
# It's hard to call directly, so instead call sre_compile.compile which (indirectly) calls _parse()

import sre_compile

for i in xrange(1000):
    sre_compile.compile("", 0)
