# This is the same as incremental_tb_bjit.py but tests the baseline JIT.
try:
    import __pyston__
    __pyston__.setOption("REOPT_THRESHOLD_INTERPRETER", 1)
except:
    pass

import incremental_tb
