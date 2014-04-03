# run_args: -n


class O(object):
    pass
o = O()

def _add():
    return o.x + o.y
def add(x, y):
    o.x = x
    o.y = y
    return _add()

print add(0, 1)
print add(0, 2.0)

print add(0, 3)
print add(1.0, 4)

print add(0, 5)
print add(1.0, 6.0)


class C(object):
    def __init__(self, n):
        self.n = n

def add1(self, rhs):
    print "add1"
    return self.n + rhs.n
def add2(self, rhs):
    print "add2"
    return -self.n + rhs.n
def add3(self, rhs):
    print "add3"
    if rhs.n == 6:
        return NotImplemented
    return self.n + rhs.n

class D(object):
    def __init__(self, n):
        self.n = n
    def __radd__(self, rhs):
        print "radd"
        return 1 + self.n + rhs.n

C.__add__ = add1
print add(C(1), C(1))
print add(C(1.0), C(2))
C.__add__ = add2
print add(C(1), C(3))
print add(C(1), D(4))
C.__add__ = add3
print add(C(1), D(5))
print add(C(1), D(6)) # this one, but not the ones above or below, should call radd!
print add(C(1), D(7))



class V(object):
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other):
        print self.x, other.x, self.y, other.y, self.z, other.z
        return self.x * other.x + self.y * other.y + self.z * other.z

v0 = V(1, 1, 1)
# Calling this first will establish all the dot() operations as [int,int]
print v0.dot(v0)

v = V(1, 1.0, 1)
# But here we give it floats, which could mess it up
print v.dot(v0)
print v0.dot(v)
print v.dot(v)
