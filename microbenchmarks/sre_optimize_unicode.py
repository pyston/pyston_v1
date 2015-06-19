import sre_compile
import sre_constants

def identity(o):
    return o

charset = [(sre_constants.RANGE, (128, 65535))]

for i in xrange(100):
    sre_compile._optimize_unicode(charset, identity)
    # print sre_compile._optimize_charset(charset, identity)
