# Making sure that classes can stay mutable:

class C(object):
    def f(self):
        return 1

    def __add__(self, rhs):
        print "C.__add__",
        return 1 + rhs

c = C()

def new_add(self, rhs):
    print "new_add",
    return 2 + rhs

n = 10
while n:
    n = n - 1
    print n, c + n
    if n == 6:
        C.__add__ = new_add
