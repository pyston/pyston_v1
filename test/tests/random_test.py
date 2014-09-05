# allow-warning: converting unicode literal to str
# skip-if: IMAGE != 'pyston_dbg'
# fail-if: '-n' in EXTRA_JIT_ARGS or '-O' in EXTRA_JIT_ARGS
# - failing to rewrite

import random

print type(random.random())
