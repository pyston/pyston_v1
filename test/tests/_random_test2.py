# skip-if: IMAGE != 'pyston_dbg'
# fail-if: '-n' in EXTRA_JIT_ARGS or '-O' in EXTRA_JIT_ARGS
# - failing to rewrite

import _random

class Random(_random.Random):
    def __init__(self, a=None):
        super(Random, self).seed(a)

for i in xrange(100):
    r = Random(i)
    print r.getrandbits(100)
