import strop

print repr(strop.whitespace)

import string
print repr(string.whitespace)
print repr(string.lowercase)
print repr(string.uppercase)

all_chars = ''.join(chr(i) for i in xrange(256))
print repr(strop.lower(all_chars))
