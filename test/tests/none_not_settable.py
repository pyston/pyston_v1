# expected: fail
# setattr() not implemented

class C(object):
    def print_none(self):
        print None
c = C()
c.print_none()
# Can't do this:
# c.None = 1
setattr(C, "None", 1)
print dir(C)
print C.None # prints None!
c.print_none() # prints None!

import sys
m = sys.modules["__main__"]
setattr(m, "None", 1)
print None # prints None!

# # this throws a syntax error:
# def f(None):
    # print None
# f()
