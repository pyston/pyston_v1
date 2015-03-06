class MyStr(str):
    pass

s = MyStr(1)
print repr(s)

import sys
sys.stdout.write(s)
