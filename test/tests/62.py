# Stackmaps don't get emitted for code that's dead even if it exists in IR:

class C(object):
    pass

c = C()
if 1 == 0:
    c.x = 1
