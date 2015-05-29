import sre_compile
import sre_constants
import re

IN = sre_constants.IN

def _identity(x):
    return x

_cache = {}
DEBUG = 1
def _compile(*key):
    # print "re._compile", key
    # internal: compile pattern
    pattern, flags = key
    bypass_cache = flags & DEBUG
    if not bypass_cache:
        cachekey = (type(key[0]),) + key
        p = _cache.get(cachekey)
        if p is not None:
            # print "got from cache"
            return p

    p = (1, 0)
    _cache[cachekey] = p
    return p

for i in xrange(1000000):
    # sre_compile._optimize_charset([('negate', None), ('literal', 34), ('literal', 92)], _identity)
    # sre_compile._compile([17, 4, 0, 3, 0, 29, 12, 0, 4294967295L, 15, 7, 26, 19, 34, 19, 92, 0, 1, 28, 0, 0, 4294967295L, 7, 6, 19, 92, 2, 18, 15, 13, 19, 34, 5, 7, 0, 19, 34, 19, 34, 1, 18, 2, 0, 29, 0, 0, 4294967295L], [(IN, [('negate', None), ('literal', 34), ('literal', 92)])], 0)
    # sre_compile._compile([17, 8, 3, 1, 1, 1, 1, 97, 0], [('literal', 97)], 0)
    _compile("a", 0)
