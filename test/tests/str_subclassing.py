class MyStr(str):
    pass

s = MyStr(1)
print repr(s)

import sys
sys.stdout.write(s)
print

print repr("hello" + MyStr("world"))
print int(MyStr("2"))
