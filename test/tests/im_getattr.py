# expected: fail
# - not implemented

# Instance methods forward any missed attribute lookups to their underlying function

class C(object):
    pass

def f(self):
    pass

C.f = f
c = C()
im = c.f

print type(f), type(im)
f.a = 1
print im.a
