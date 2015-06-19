# Metaclass test:

class M(type):
    a = 1

class C(object):
    __metaclass__ = M
    b = 2

o = C()
o.c = 3

print M.a
print C.b
print C.a
print o.c
print o.b
try:
    print o.a
except AttributeError, e:
    print e
