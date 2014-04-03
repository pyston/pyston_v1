def f(c):
    c.x = None
    c.x = 1
    # return None

class C(object):
    pass
print f(C())
