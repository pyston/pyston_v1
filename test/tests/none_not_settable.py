# expected: fail
# - not implemented yet

import sys
m = sys.modules["__main__"]
setattr(m, "None", 1)
print None # prints None!

# # this throws a syntax error:
# def f(None):
    # print None
# f()
