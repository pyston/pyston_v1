# Checking how functions get references to their modules

import sys
m = sys.modules['__main__']

class C(object):
    pass
print C

m.__name__ = "test"

print C
print C.__module__ # should still be "__main__"
class C(object):
    pass
print C
print C.__module__ # should now be "test"

