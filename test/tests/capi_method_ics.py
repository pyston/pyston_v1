# statcheck: noninit_count("slowpath_runtimecall") <= 10
# statcheck: noninit_count("slowpath_callattr") <= 10
# statcheck: noninit_count("slowpath_callfunc") <= 10
# run_args: -n

import math
for i in xrange(10000):
    math.copysign(1, -1)
