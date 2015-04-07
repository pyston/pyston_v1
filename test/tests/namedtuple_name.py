# expected: fail
# - needs sys._getframe

import collections
NT = collections.namedtuple("NT", ["field1", "field2"])
print NT
