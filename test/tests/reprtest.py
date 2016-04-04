a=1
try:
    a.b
except AttributeError, e:
    print repr(e)

print repr("both\'\"quotes")
print repr("single\'quote")
print repr("double\"quote")

import re
def p(o):
    s = repr(o)
    s = re.sub("0x[0-9a-fA-F]+", "0x0", s)
    return s

def foo():
    pass
class C(object):
    def a(self):
        pass
print p(C.a), p(C().a)
print p(C.__str__), p(C().__str__)
print p(type(u"").find)
print p(foo)
foo.__name__ = "bar"
print p(foo)
print p(sorted)
