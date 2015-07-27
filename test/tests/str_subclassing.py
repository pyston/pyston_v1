class MyStr(str):
    pass

s = MyStr(1)
print repr(s)

import sys
sys.stdout.write(s)
print

print repr("hello" + MyStr("world"))
print int(MyStr("2"))

class MyStr(str):
    def __init__(*args):
        print "MyStr.__init__", map(type, args)

class C(object):
    def __str__(self):
        return MyStr("hello world")

print type(str(C()))
m = MyStr(C())
print type(m), repr(m)
