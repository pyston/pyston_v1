# expected: fail
# - see cfg.cpp::getLastLinenoSub()

import sys

def f():
    1/ (len((1/1,
            1/1,
            1/1)) - 3)

try:
    f()
    assert 0
except ZeroDivisionError:
    print sys.exc_info()[2].tb_next.tb_lineno
    print sys.exc_info()[2].tb_next.tb_frame.f_lineno
