class Vector(object):
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def __add__(self, rhs):
        return Vector(self.x + rhs.x, self.y + rhs.y, self.z + rhs.z)

    def dot(self, rhs):
        return self.x * rhs.x + self.y * rhs.y + self.z * rhs.z

def f(n):
    v = Vector(0,0.1,0)
    a = Vector(1.0, 1.1, 1.2)
    t = 0
    for i in xrange(n):
        t = t + v.dot(a)
    return t
print f(10000000)

