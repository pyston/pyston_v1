# expected: fail
# - can't pass exceptions through C API yet
# (TODO fold this into reduce.py once it works)

import operator

try:
    print reduce(operator.add, "hello world", 0)
except TypeError, e:
    print e

