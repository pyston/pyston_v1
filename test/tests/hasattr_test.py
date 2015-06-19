# hasattr() swallows all most (but not all) exceptions

import __builtin__

class C(object):
    def __getattr__(self, attr):
        raise getattr(__builtin__, attr)()

c = C()
print hasattr(c, 'a')
c.a = 1
print hasattr(c, 'a')
print hasattr(c, 'ZeroDivisionError')
print hasattr(c, 'RuntimeError')
print hasattr(c, 'SystemError')
try:
    print hasattr(c, 'KeyboardInterrupt')
except KeyboardInterrupt:
    print "caught KeyboardInterrupt"
