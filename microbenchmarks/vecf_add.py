class Vector(object):
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def __add__(self, rhs):
        return Vector(self.x + rhs.x, self.y + rhs.y, self.z + rhs.z)

    def __str__(self):
        return "Vector(%f, %f, %f)" % (self.x, self.y, self.z)

def f(n):
    v = Vector(0,0,0)
    a = Vector(1.0, 1.1, 1.2)
    # b = Vector(1, -2, 1)
    for i in xrange(n):
        # if v.y > 0:
            # v = v + b
        v = v + a
    return v
print f(10000000)
